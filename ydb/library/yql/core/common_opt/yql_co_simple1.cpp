#include "yql_co.h"
#include "yql_co_sqlin.h"

#include <ydb/library/yql/core/yql_atom_enums.h>
#include <ydb/library/yql/core/yql_expr_type_annotation.h>
#include <ydb/library/yql/core/yql_join.h>
#include <ydb/library/yql/core/yql_opt_utils.h>
#include <ydb/library/yql/core/yql_type_helpers.h>
#include <ydb/library/yql/core/yql_expr_optimize.h>

#include <ydb/library/yql/utils/log/log.h>

#include <util/generic/map.h>
#include <util/generic/bitmap.h>
#include <util/string/cast.h>
#include <util/generic/xrange.h>

#include <algorithm>
#include <iterator>

namespace NYql {

namespace {

using namespace NNodes;

template<typename TInt>
class TMinAggregate {
public:
    TInt operator() (TInt cur, TInt value) { return std::min(cur, value); }
};

template<typename TInt>
class TMaxAggregate {
public:
    TInt operator() (TInt cur, TInt value) { return std::max(cur, value); }
};

bool CanRewriteToEmptyContainer(const TExprNode& src) {
    if (auto multi = src.GetConstraint<TMultiConstraintNode>()) {
        for (auto& item: multi->GetItems()) {
            for (auto c: item.second.GetAllConstraints()) {
                if (c->GetName() != TEmptyConstraintNode::Name()) {
                    return false;
                }
            }
        }
    }
    return true;
}

TExprNode::TPtr KeepSortedConstraint(TExprNode::TPtr node, const TSortedConstraintNode* sorted, TExprContext& ctx) {
    if (!sorted) {
        return node;
    }
    const auto& constent = sorted->GetContent();
    return ctx.Builder(node->Pos())
        .Callable("AssumeSorted")
            .Add(0, std::move(node))
            .List(1)
                .Do([&](TExprNodeBuilder& parent) -> TExprNodeBuilder& {
                    size_t index = 0;
                    for (auto c : constent) {
                        parent.Callable(index++, "Bool")
                            .Atom(0, ToString(c.second), TNodeFlags::Default)
                        .Seal();
                    }
                    return parent;
                })
            .Seal()
            .Lambda(2)
                .Param("item")
                .List()
                    .Do([&](TExprNodeBuilder& parent) -> TExprNodeBuilder& {
                        size_t index = 0;
                        for (auto c : constent) {
                            parent.Callable(index++, "Member")
                                .Arg(0, "item")
                                .Atom(1, c.first.front())
                            .Seal();
                        }
                        return parent;
                    })
                .Seal()
            .Seal()
        .Seal()
        .Build();
}

TExprNode::TPtr KeepConstraints(TExprNode::TPtr node, const TExprNode& src, TExprContext& ctx) {
    auto res = KeepSortedConstraint(node, src.GetConstraint<TSortedConstraintNode>(), ctx);
    if (auto uniq = src.GetConstraint<TUniqueConstraintNode>()) {
        res = ctx.Builder(node->Pos())
            .Callable("AssumeUnique")
                .Add(0, std::move(res))
                .List(1)
                    .Do([&](TExprNodeBuilder& parent) -> TExprNodeBuilder& {
                        size_t index = 0;
                        for (auto col: uniq->GetColumns()) {
                            parent.Atom(index++, ToString(col), TNodeFlags::Default);
                        }
                        return parent;
                    })
                .Seal()
            .Seal()
            .Build();
    }
    return res;
}

template<typename TInt>
bool ConstIntAggregate(const TExprNode::TChildrenType& values, std::function<TInt(TInt, TInt)> aggFunc,
    TInt& result)
{
    auto extractValue = [&values] (size_t index, TInt& value) {
        if (!TMaybeNode<TCoIntegralCtor>(values[index])) {
            return false;
        }

        ui64 extracted;
        bool hasSign;
        bool isSigned;
        ExtractIntegralValue(*values[index], false, hasSign, isSigned, extracted);
        value = static_cast<TInt>(hasSign ? -extracted : extracted);
        return true;
    };

    if (values.size() == 0) {
        return false;
    }
    if (!extractValue(0, result)) {
        return false;
    }

    for (ui32 i = 1; i < values.size(); ++i) {
        TInt value;
        if (!extractValue(i, value)) {
            return false;
        }

        result = aggFunc(result, value);
    }

    return true;
}

template<template<typename> class TAgg>
TExprNode::TPtr ConstFoldNodeIntAggregate(const TExprNode::TPtr& node, TExprContext& ctx) {
    auto dataSlot = node->GetTypeAnn()->Cast<TDataExprType>()->GetSlot();

    if (dataSlot == EDataSlot::Uint64) {
        ui64 result;
        if (ConstIntAggregate<ui64>(node->Children(), TAgg<ui64>(), result)) {
            return ctx.NewCallable(node->Pos(), node->GetTypeAnn()->Cast<TDataExprType>()->GetName(), {ctx.NewAtom(node->Pos(), ToString(result))});
        }
    } else {
        i64 result;
        if (ConstIntAggregate<i64>(node->Children(), TAgg<i64>(), result)) {
            return ctx.NewCallable(node->Pos(), node->GetTypeAnn()->Cast<TDataExprType>()->GetName(), {ctx.NewAtom(node->Pos(), ToString(result))});
        }
    }

    return node;
}

TExprNode::TPtr ExpandPositionalUnionAll(const TExprNode& node, const TVector<TColumnOrder>& columnOrders,
    TExprNode::TListType children, TExprContext& ctx, TOptimizeContext& optCtx) {
    auto targetColumnOrder = optCtx.Types->LookupColumnOrder(node);
    YQL_ENSURE(targetColumnOrder);

    for (ui32 childIndex = 0; childIndex < children.size(); ++childIndex) {
        const auto& childColumnOrder = columnOrders[childIndex];
        auto& child = children[childIndex];
        if (childColumnOrder == *targetColumnOrder) {
            continue;
        }

        YQL_ENSURE(childColumnOrder.size() == targetColumnOrder->size());
        child = ctx.Builder(child->Pos())
            .Callable("Map")
                .Add(0, child)
                .Lambda(1)
                    .Param("row")
                    .Callable("AsStruct")
                    .Do([&](TExprNodeBuilder &parent) -> TExprNodeBuilder & {
                        for (size_t i = 0; i < childColumnOrder.size(); ++i) {
                            parent
                                .List(i)
                                    .Atom(0, child->Pos(), (*targetColumnOrder)[i])
                                    .Callable(1, "Member")
                                        .Arg(0, "row")
                                        .Atom(1, childColumnOrder[i])
                                    .Seal()
                                .Seal();
                        }
                        return parent;
                    })
                    .Seal()
                .Seal()
            .Seal()
            .Build();
    }

    auto res = ctx.NewCallable(node.Pos(), "UnionAll", std::move(children));
    return KeepColumnOrder(res, node, ctx, *optCtx.Types);
}

TExprNode::TPtr ExpandFlattenEquiJoin(const TExprNode::TPtr& node, TExprContext& ctx) {
    auto settings = node->Children().back();
    TExprNode::TListType settingsChildren;
    bool hasFlatten = false;
    for (auto& child : settings->Children()) {
        if (child->ChildrenSize() > 0 && child->Head().Content() == "flatten") {
            hasFlatten = true;
            continue;
        }

        settingsChildren.push_back(child);
    }

    if (!hasFlatten) {
        return node;
    }

    const size_t numLists = node->ChildrenSize() - 2;
    TJoinLabels labels;
    for (ui32 idx = 0; idx < numLists; ++idx) {
        const auto& listPair = *node->Child(idx);
        const auto& list = listPair.Head();
        const TTypeAnnotationNode* itemType = list.GetTypeAnn()->Cast<TListExprType>()->GetItemType();
        auto structType = itemType->Cast<TStructExprType>();
        if (auto err = labels.Add(ctx, *listPair.Child(1), structType)) {
            ctx.AddError(*err);
            return nullptr;
        }
    }

    auto joins = node->Child(node->ChildrenSize() - 2);
    auto columnTypes = GetJoinColumnTypes(*joins, labels, ctx);
    TMap<TString, std::pair<bool, TVector<TString>>> remap; // column -> isOptional, list of columns
    for (auto it : labels.Inputs) {
        for (auto item : it.InputType->GetItems()) {
            TString fullName = it.FullName(item->GetName());
            auto type = columnTypes.FindPtr(fullName);
            if (type) {
                auto columnName = it.ColumnName(fullName);
                auto iter = remap.find(columnName);
                if (iter != remap.end()) {
                    if (iter->second.first) {
                        // still optional
                        if ((*type)->GetKind() == ETypeAnnotationKind::Optional) {
                            iter->second.second.push_back(fullName);
                        } else {
                            iter->second.first = false;
                            iter->second.second.clear();
                            iter->second.second.push_back(fullName);
                        }
                    }
                    continue;
                }

                remap.emplace(TString(columnName), std::make_pair((*type)->GetKind() == ETypeAnnotationKind::Optional,
                    TVector<TString>(1, fullName)));
            }
        }
    }

    auto lambdaArg = ctx.NewArgument(node->Pos(), "row");
    TExprNode::TListType remapItems;
    for (auto& x : remap) {
        TExprNode::TPtr value;
        if (x.second.second.size() == 1) {
            value = ctx.Builder(node->Pos())
                .Callable("Member")
                    .Add(0, lambdaArg)
                    .Atom(1, x.second.second.front())
                .Seal()
                .Build();
        } else {
            TExprNode::TListType values;
            for (auto& column : x.second.second) {
                values.push_back(ctx.Builder(node->Pos())
                    .Callable("Member")
                        .Add(0, lambdaArg)
                        .Atom(1, column)
                    .Seal()
                    .Build());
            }

            value = ctx.NewCallable(node->Pos(), "Coalesce", std::move(values));
        }

        remapItems.push_back(ctx.NewList(node->Pos(), { ctx.NewAtom(node->Pos(), x.first), value }));
    }

    auto lambdaBody = ctx.NewCallable(node->Pos(), "AsStruct", std::move(remapItems));
    auto mapLambda = ctx.NewLambda(node->Pos(), ctx.NewArguments(node->Pos(), { lambdaArg }), std::move(lambdaBody));

    auto newSettings = ctx.ChangeChildren(*settings, std::move(settingsChildren));
    auto newJoin = ctx.ChangeChild(*node, node->ChildrenSize() - 1, std::move(newSettings));
    return ctx.NewCallable(node->Pos(), "Map", { std::move(newJoin), std::move(mapLambda) });
}

void GatherEquiJoinKeyColumnsFromEquality(TExprNode::TPtr columns, THashSet<TString>& keyColumns) {
    for (ui32 i = 0; i < columns->ChildrenSize(); i += 2) {
        auto table = columns->Child(i)->Content();
        auto column = columns->Child(i + 1)->Content();
        keyColumns.insert({ FullColumnName(table, column) });
    }
}

void GatherEquiJoinKeyColumns(TExprNode::TPtr joinTree, THashSet<TString>& keyColumns) {
    auto left = joinTree->Child(1);
    if (!left->IsAtom()) {
        GatherEquiJoinKeyColumns(left, keyColumns);
    }

    auto right = joinTree->Child(2);
    if (!right->IsAtom()) {
        GatherEquiJoinKeyColumns(right, keyColumns);
    }

    auto leftColumns = joinTree->Child(3);
    auto rightColumns = joinTree->Child(4);
    GatherEquiJoinKeyColumnsFromEquality(leftColumns, keyColumns);
    GatherEquiJoinKeyColumnsFromEquality(rightColumns, keyColumns);
}

void GatherDroppedSingleTableColumns(TExprNode::TPtr joinTree, const TJoinLabels& labels, TSet<TString>& drops) {
    auto left = joinTree->Child(1);
    auto right = joinTree->Child(2);
    if (!left->IsAtom()) {
        GatherDroppedSingleTableColumns(left, labels, drops);
    }

    if (!right->IsAtom()) {
        GatherDroppedSingleTableColumns(right, labels, drops);
    }

    auto mode = joinTree->Head().Content();
    TExprNode::TPtr columns = nullptr;
    if (mode == "LeftSemi" || mode == "LeftOnly") {
        // drop right table columns
        columns = joinTree->Child(4);
    }
    else if (mode == "RightSemi" || mode == "RightOnly") {
        // drop left table columns
        columns = joinTree->Child(3);
    }

    if (columns) {
        auto label = *labels.FindInput(columns->Head().Content());
        for (auto column : label->EnumerateAllColumns()) {
            drops.insert(column);
        }
    }
}

TExprNode::TPtr RemoveDeadPayloadColumns(const TExprNode::TPtr& node, TExprContext& ctx) {
    auto settings = node->Children().back();
    TSet<TString> drops;
    for (auto& setting : settings->Children()) {
        auto name = setting->Head().Content();
        if (name == "rename") {
            if (setting->Child(2)->Content().empty()) {
                drops.insert(TString(setting->Child(1)->Content()));
            }
        }
    }

    for (auto& setting : settings->Children()) {
        auto name = setting->Head().Content();
        if (name == "rename") {
            if (!setting->Child(2)->Content().empty()) {
                drops.erase(TString(setting->Child(1)->Content()));
            }
        }
    }

    TJoinLabels labels;
    for (ui32 i = 0; i < node->ChildrenSize() - 2; ++i) {
        auto err = labels.Add(ctx, *node->Child(i)->Child(1),
            node->Child(i)->Head().GetTypeAnn()->Cast<TListExprType>()
            ->GetItemType()->Cast<TStructExprType>());
        if (err) {
            ctx.AddError(*err);
            return nullptr;
        }
    }

    auto joinTree = node->Child(node->ChildrenSize() - 2);
    GatherDroppedSingleTableColumns(joinTree, labels, drops);
    if (drops.empty()) {
        return node;
    }

    THashSet<TString> keyColumns;
    GatherEquiJoinKeyColumns(joinTree, keyColumns);
    for (auto& keyColumn : keyColumns) {
        drops.erase(keyColumn);
    }

    if (drops.empty()) {
        return node;
    }

    TExprNode::TListType nodeChildren(node->ChildrenList());

    std::vector<std::vector<TString>> separated;
    separated.reserve(labels.Inputs.size());
    for (const auto& input : labels.Inputs) {
        separated.emplace_back();
        for (const auto& column : input.EnumerateAllColumns()) {
            if (drops.end() == drops.find(column)) {
                TStringBuf part1, part2;
                SplitTableName(column, part1, part2);
                separated.back().emplace_back(input.MemberName(part1, part2));
            }
        }
    }

    for (ui32 j = 0U; j < separated.size(); ++j) {
        const auto& good = separated[j];
        TExprNode::TListType dropChildren(nodeChildren[j]->ChildrenList());
        dropChildren.front() = ctx.Builder(node->Pos())
            .Callable("ExtractMembers")
                .Add(0, std::move(dropChildren.front()))
                .List(1)
                    .Do([&good](TExprNodeBuilder& builder) -> TExprNodeBuilder& {
                        for (ui32 i = 0U; i < good.size(); ++i) {
                            builder.Atom(i, good[i]);
                        }
                        return builder;
                    })
                .Seal()
            .Seal()
            .Build();

        nodeChildren[j] = ctx.ChangeChildren(*nodeChildren[j], std::move(dropChildren));
    }

    TExprNode::TListType settingsChildren;
    for (const auto& setting : settings->Children()) {
        auto name = setting->Head().Content();
        if (name != "rename" || !setting->Child(2)->Content().empty() || !drops.contains(setting->Child(1)->Content())) {
            settingsChildren.push_back(setting);
        }
    }

    nodeChildren.back() = ctx.NewList(settings->Pos(), std::move(settingsChildren));
    return ctx.ChangeChildren(*node, std::move(nodeChildren));
}

TExprNode::TPtr HandleEmptyListInJoin(const TExprNode::TPtr& node, TExprContext& ctx, const TTypeAnnotationContext& typeCtx) {
    TMaybe<TJoinLabels> labels;
    for (ui32 inputIndex = 0; inputIndex < node->ChildrenSize() - 2; ++inputIndex) {
        auto& input = SkipCallables(node->Child(inputIndex)->Head(), SkippableCallables);
        if (!IsEmptyContainer(input) && !IsEmpty(input, typeCtx)) {
            continue;
        }

        auto joinTree = node->Child(node->ChildrenSize() - 2);
        if (!labels) {
            labels.ConstructInPlace();
            for (ui32 i = 0; i < node->ChildrenSize() - 2; ++i) {
                auto err = labels->Add(ctx, *node->Child(i)->Child(1),
                    node->Child(i)->Head().GetTypeAnn()->Cast<TListExprType>()
                    ->GetItemType()->Cast<TStructExprType>());
                if (err) {
                    ctx.AddError(*err);
                    return nullptr;
                }
            }
        }

        if (IsRequiredSide(joinTree, *labels, inputIndex).first) {
            return ctx.NewCallable(node->Pos(), "List", {ExpandType(node->Pos(), *node->GetTypeAnn(), ctx)});
        }
    }

    return node;
}

TExprNode::TPtr UpdateJoinTreeUniqueRecursive(const TExprNode::TPtr& joinTree, const TJoinLabels& labels, const TVector<const TUniqueConstraintNode*>& unique, TExprContext& ctx) {
    TExprNode::TPtr res = joinTree;

    TEquiJoinLinkSettings linkSettings = GetEquiJoinLinkSettings(*joinTree->Child(5));
    bool updateSettings = false;
    auto left = joinTree->ChildPtr(1);
    if (!left->IsAtom()) {
        left = UpdateJoinTreeUniqueRecursive(left, labels, unique, ctx);
        if (left != joinTree->ChildPtr(1)) {
            res = ctx.ChangeChild(*res, 1, std::move(left));
        }
    } else if (linkSettings.LeftHints.find("unique") == linkSettings.LeftHints.end()) {
        if (auto label = labels.FindInput(left->Content())) {
            if (auto ndx = labels.FindInputIndex(left->Content())) {
                if (auto u = unique[*ndx]) {
                    auto keys = joinTree->Child(3);
                    THashSet<TString> keySet;
                    for (ui32 i = 0; i < keys->ChildrenSize(); i += 2) {
                        keySet.insert((*label)->MemberName(keys->Child(i)->Content(), keys->Child(i + 1)->Content()));
                    }
                    if (AllOf(u->GetColumns(), [&keySet](TStringBuf col) { return keySet.find(col) != keySet.end(); })) {
                        linkSettings.LeftHints.insert("unique");
                        updateSettings = true;
                    }
                }
            }
        }
    }

    auto right = joinTree->ChildPtr(2);
    if (!right->IsAtom()) {
        right = UpdateJoinTreeUniqueRecursive(right, labels, unique, ctx);
        if (right != joinTree->ChildPtr(2)) {
            res = ctx.ChangeChild(*res, 2, std::move(right));
        }
    } else if (linkSettings.RightHints.find("unique") == linkSettings.RightHints.end()) {
        if (auto label = labels.FindInput(right->Content())) {
            if (auto ndx = labels.FindInputIndex(right->Content())) {
                if (auto u = unique[*ndx]) {
                    auto keys = joinTree->Child(4);
                    THashSet<TString> keySet;
                    for (ui32 i = 0; i < keys->ChildrenSize(); i += 2) {
                        keySet.insert((*label)->MemberName(keys->Child(i)->Content(), keys->Child(i + 1)->Content()));
                    }
                    if (AllOf(u->GetColumns(), [&keySet](TStringBuf col) { return keySet.find(col) != keySet.end(); })) {
                        linkSettings.RightHints.insert("unique");
                        updateSettings = true;
                    }
                }
            }
        }
    }

    if (updateSettings) {
        res = ctx.ChangeChild(*res, 5, BuildEquiJoinLinkSettings(linkSettings, ctx));
    }

    return res;
}


TExprNode::TPtr HandleUniqueListInJoin(const TExprNode::TPtr& node, TExprContext& ctx, const TTypeAnnotationContext& typeCtx) {
    if (!typeCtx.IsConstraintCheckEnabled<TUniqueConstraintNode>()) {
        return node;
    }

    TJoinLabels labels;
    TVector<const TUniqueConstraintNode*> unique;
    unique.reserve(node->ChildrenSize() - 2);
    for (ui32 i = 0; i < node->ChildrenSize() - 2; ++i) {
        auto err = labels.Add(ctx, *node->Child(i)->Child(1),
            node->Child(i)->Head().GetTypeAnn()->Cast<TListExprType>()->GetItemType()->Cast<TStructExprType>());
        if (err) {
            ctx.AddError(*err);
            return nullptr;
        }
        unique.push_back(node->Child(i)->Head().GetConstraint<TUniqueConstraintNode>());
    }

    auto joinTree = UpdateJoinTreeUniqueRecursive(node->ChildPtr(node->ChildrenSize() - 2), labels, unique, ctx);
    if (joinTree != node->ChildPtr(node->ChildrenSize() - 2)) {
        return ctx.ChangeChild(*node, node->ChildrenSize() - 2, std::move(joinTree));
    }

    return node;
}

bool IsDataType(const TTypeAnnotationNode& type) {
    return type.GetKind() == ETypeAnnotationKind::Data;
}

bool IsDataType(const TExprNode& node) {
    return node.GetTypeAnn() && IsDataType(*node.GetTypeAnn());
}

bool IsBoolType(const TTypeAnnotationNode& type) {
    return type.GetKind() == ETypeAnnotationKind::Data
        && type.Cast<TDataExprType>()->GetSlot() == EDataSlot::Bool;
}

bool IsBoolType(const TExprNode& node) {
    return node.GetTypeAnn() && IsBoolType(*node.GetTypeAnn());
}

bool IsOptBoolType(const TTypeAnnotationNode& type) {
    return type.GetKind() == ETypeAnnotationKind::Optional
        && IsBoolType(*type.Cast<TOptionalExprType>()->GetItemType());
}

bool IsOptBoolType(const TExprNode& node) {
    return node.GetTypeAnn() && IsOptBoolType(*node.GetTypeAnn());
}

template <bool AppendOrPrepend>
TExprNode::TPtr OptimizeInsert(const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) {
    const auto& list = AppendOrPrepend ? node->Head() : node->Tail();
    if (IsEmptyContainer(list) || IsEmpty(list, *optCtx.Types)) {
        YQL_CLOG(DEBUG, Core) << node->Content() << " over empty " << list.Content();
        return ctx.NewCallable(node->Pos(), "AsList", {AppendOrPrepend ? node->TailPtr() : node->HeadPtr()});
    }

    if (list.IsCallable("AsList")) {
        YQL_CLOG(DEBUG, Core) << node->Content() << " over " << list.Content();
        auto children = list.ChildrenList();
        if (AppendOrPrepend) {
            children.emplace_back(node->TailPtr());
        } else {
            children.emplace(children.cbegin(), node->HeadPtr());
        }
        return ctx.ChangeChildren(list, std::move(children));
    }
    return node;
}

template <bool Ordered>
TExprNode::TPtr ExpandExtract(const TExprNode::TPtr& node, TExprContext& ctx) {
    YQL_CLOG(DEBUG, Core) << "Expand " << node->Content();
    const bool isStruct = ETypeAnnotationKind::Struct == GetSeqItemType(node->Head().GetTypeAnn())->GetKind();
    return ctx.Builder(node->Pos())
        .Callable(Ordered ? "OrderedMap" : "Map")
            .Add(0, node->HeadPtr())
            .Lambda(1)
                .Param("x")
                .Callable(isStruct ? "Member" : "Nth")
                    .Arg(0, "x")
                    .Add(1, node->TailPtr())
                .Seal()
            .Seal()
        .Seal().Build();
}

std::vector<TExprNode::TListType> GroupNodeChildrenByType(const TExprNode::TPtr& node) {
    std::vector<TExprNode::TListType> groups;
    std::map<const TTypeAnnotationNode*, ui32> typeToGroup;
    for (ui32 i = 0; i < node->ChildrenSize(); ++i) {
        auto child = node->Child(i);
        auto groupIndex = typeToGroup.emplace(child->GetTypeAnn(), groups.size()).first->second;
        if (groupIndex >= groups.size()) {
            YQL_ENSURE(groupIndex == groups.size());
            groups.resize(groupIndex + 1);
        }
        groups[groupIndex].push_back(child);
    }
    return groups;
}

template <bool Ordered>
TExprNode::TPtr ExpandUnionAll(const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) {
    YQL_CLOG(DEBUG, Core) << "Expand " << node->Content();
    if (node->ChildrenSize() == 1) {
        return node->HeadPtr();
    }

    auto resultStructType = node->GetTypeAnn()->Cast<TListExprType>()->GetItemType()->Cast<TStructExprType>();
    TVector<TExprNode::TPtr> nulls(resultStructType->GetSize());
    auto remapList = [&ctx, &nulls, resultStructType](TExprNode::TPtr input, const TTypeAnnotationNode* inputType) -> TExprNode::TPtr {
        auto pos = input->Pos();
        auto arg = ctx.NewArgument(pos, "item");
        auto inputStructType = inputType->Cast<TListExprType>()->GetItemType()->Cast<TStructExprType>();
        TExprNode::TListType bodyItems;
        ui32 resultIndex = 0;
        for (auto& item : resultStructType->GetItems()) {
            auto resultType = item->GetItemType();
            auto name = ctx.NewAtom(pos, item->GetName());
            TExprNode::TPtr member = nullptr;
            TMaybe<TIssue> err;
            if (resultType->GetKind() == ETypeAnnotationKind::Error) {
                err = resultType->Cast<TErrorExprType>()->GetError();
            }

            if (!err) {
                auto myPos = inputStructType->FindItem(item->GetName());
                if (!myPos) {
                    auto& nullNode = nulls[resultIndex];
                    if (!nullNode) {
                        nullNode = ExpandType(pos, *resultType, ctx);
                    }

                    member = ctx.NewCallable(pos, "Nothing", { nullNode });
                }
                else {
                    auto myType = inputStructType->GetItems()[*myPos]->GetItemType();
                    member = ctx.NewCallable(pos, "Member", { arg, name });
                    if (TrySilentConvertTo(member, *myType, *resultType, ctx) == IGraphTransformer::TStatus::Error) {
                        err = TIssue(
                            ctx.GetPosition(pos),
                            TStringBuilder()
                            << "Uncompatible member " << item->GetName() << " types: "
                            << *myType << " and " << *resultType
                        );
                    }
                }
            }

            if (err) {
                member = ctx.NewCallable(pos, "Error", { ExpandType(pos, *ctx.MakeType<TErrorExprType>(*err), ctx) });
            }

            bodyItems.push_back(ctx.NewList(pos, { name, member }));
            ++resultIndex;
        }

        auto body = ctx.NewCallable(pos, "AsStruct", std::move(bodyItems));
        return ctx.NewCallable(pos, Ordered ? "OrderedMap" : "Map", { input, ctx.NewLambda(
            pos,
            ctx.NewArguments(pos, { arg }),
            std::move(body)
        ) });
    };

    TExprNode::TListType remappedList;
    // group children by ann type and preserve order for stability in tests
    std::vector<TExprNode::TListType> groups = GroupNodeChildrenByType(node);
    for (auto& group : groups) {
        YQL_ENSURE(!group.empty());

        auto typeAnn = group[0]->GetTypeAnn();
        TExprNode::TPtr remapped;
        if (group.size() == 1) {
            remapped = remapList(group[0], typeAnn);
        } else {
            auto pos = group[0]->Pos();
            remapped = remapList(ctx.NewCallable(pos, Ordered ? "Merge" : "Extend", std::move(group)), typeAnn);
        }

        if (!remapped) {
            return node;
        }

        remappedList.push_back(remapped);
    }

    auto res = ctx.NewCallable(node->Pos(), Ordered ? "Merge" : "Extend", std::move(remappedList));
    return KeepColumnOrder(res, *node, ctx, *optCtx.Types);
}

TExprNode::TPtr RemoveNothingFromCoalesce(const TExprNode& node, TExprContext& ctx) {
    TExprNode::TListType newChildren(node.Children().begin() + 1, node.Children().end());
    return ctx.ChangeChildren(node, std::move(newChildren));
}

TExprNode::TPtr OptimizeTryMember(const TExprNode::TPtr& node, TExprContext& ctx) {
    YQL_CLOG(DEBUG, Core) << "Optimize " << node->Content();
    const bool isStructOptional = node->Head().GetTypeAnn()->GetKind() == ETypeAnnotationKind::Optional;
    const TStructExprType* structType = RemoveOptionalType(node->Head().GetTypeAnn())->Cast<TStructExprType>();
    const bool isOptional = node->Tail().GetTypeAnn()->GetKind() == ETypeAnnotationKind::Optional;
    const auto memberName = node->Child(1)->Content();
    const auto wrappedDefault = ctx.WrapByCallableIf(isStructOptional && !node->TailPtr()->IsCallable("Null") &&
        node->TailPtr()->GetTypeAnn()->GetKind() != ETypeAnnotationKind::Optional, "Just", node->TailPtr());

    for (const auto& field : structType->GetItems()) {
        if (field->GetName() == memberName) {
            const bool just = (isStructOptional || isOptional) && field->GetItemType()->GetKind() != ETypeAnnotationKind::Optional;
            auto memberArg = isStructOptional ? ctx.NewArgument(node->Pos(), "x") : node->HeadPtr();
            auto member = ctx.NewCallable(node->Pos(), "Member", { memberArg, node->ChildPtr(1) });
            auto body = ctx.WrapByCallableIf(just, "Just", std::move(member));
            if (!isStructOptional) {
                return body;
            }

            auto lambda = ctx.NewLambda(node->Pos(), ctx.NewArguments(node->Pos(), { memberArg }), std::move(body));
            if (node->TailPtr()->IsCallable("Null")) {
                return ctx.Builder(node->Pos())
                    .Callable("FlatMap")
                        .Add(0, node->HeadPtr())
                        .Add(1, lambda)
                    .Seal()
                    .Build();
            }

            return ctx.Builder(node->Pos())
                .Callable("IfPresent")
                    .Add(0, node->HeadPtr())
                    .Add(1, lambda)
                    .Add(2, wrappedDefault)
                .Seal()
                .Build();
        }
    }

    return wrappedDefault;
}

TExprNode::TPtr RemoveOptionalReduceOverData(const TExprNode::TPtr& node, TExprContext& ctx) {
    if (node->Head().GetTypeAnn()->GetKind() != ETypeAnnotationKind::Optional) {
        YQL_CLOG(DEBUG, Core) << "Remove " << node->Content() << " over data";
        const auto& lambda = node->Tail();
        const auto& arg1 = lambda.Head().Head();
        const auto& arg2 = lambda.Head().Tail();
        return ctx.ReplaceNodes(lambda.TailPtr(), {{&arg1, node->HeadPtr()}, {&arg2, node->ChildPtr(1)}});
    }

    return node;
}

TExprNode::TPtr PropagateCoalesceWithConstIntoLogicalOps(const TExprNode::TPtr& node, TExprContext& ctx) {
    if (node->Head().IsCallable("Likely")) {
        const auto value = FromString<bool>(node->Child(1)->Head().Content());
        if (!value) {
            YQL_CLOG(DEBUG, Core) << "PropagateCoalesceWithConst over " << node->Head().Content() << " (false)";
            auto ret = ctx.Builder(node->Pos())
                .Callable("Likely")
                    .Callable(0, "Coalesce")
                        .Add(0, node->Head().HeadPtr())
                        .Add(1, node->ChildPtr(1))
                    .Seal()
                .Seal()
                .Build();
            return ret;
        }
    }

    if (node->Head().IsCallable("Not")) {
        YQL_CLOG(DEBUG, Core) << "PropagateCoalesceWithConst over Not";
        auto ret = ctx.Builder(node->Pos())
            .Callable("Not")
                .Callable(0, "Coalesce")
                    .Add(0, node->Head().HeadPtr())
                    .Callable(1, "Not")
                        .Add(0, node->ChildPtr(1))
                    .Seal()
                .Seal()
            .Seal()
            .Build();

        return ret;
    }

    if (node->Head().IsCallable({"And", "Or"})) {
        YQL_CLOG(DEBUG, Core) << "PropagateCoalesceWithConst over " << node->Head().Content();
        auto children = node->Head().ChildrenList();
        for (auto& child : children) {
            child = ctx.NewCallable(node->Pos(), node->Content(), {std::move(child), node->TailPtr()});
        }
        return ctx.NewCallable(node->Head().Pos(), node->Head().Content(), std::move(children));
    }

    return node;
}

template<bool AndOr>
TExprNode::TPtr SimplifyLogical(const TExprNode::TPtr& node, TExprContext& ctx) {
    const auto size = node->ChildrenSize();
    ui32 nothings = 0U, same = 0U, justs = 0U, negations = 0U, literals = 0U, bools = 0U;
    node->ForEachChild([&](const TExprNode& child) {
        if (child.IsCallable(node->Content()))
            ++same;
        if (child.IsCallable("Nothing"))
            ++nothings;
        if (child.IsCallable("Not"))
            ++negations;
        if (child.IsCallable("Just"))
            ++justs;
        if (child.IsCallable("Bool"))
            ++literals;
        if (IsBoolType(child))
            ++bools;
    });

    if (size == nothings) {
        YQL_CLOG(DEBUG, Core) << node->Content() <<  " over Nothing";
        return node->HeadPtr();
    }
/*TODO Move to peephole
    if (size == negations) {
        YQL_CLOG(DEBUG, Core) << node->Content() <<  " over negations";
        TExprNode::TListType children;
        children.reserve(size);
        node->ForEachChild([&](const TExprNode& child) {
            children.emplace_back(child.HeadPtr());
        });
        return ctx.NewCallable(node->Pos(), "Not", {ctx.NewCallable(node->Pos(), AndOr ? "Or" : "And" , std::move(children))});
    }
*/
    if (same) {
        YQL_CLOG(DEBUG, Core) << node->Content() <<  " over " << node->Content();
        TExprNode::TListType children;
        children.reserve(size);
        node->ForEachChild([&](TExprNode& child) {
            if (child.IsCallable(node->Content())) {
                child.ForEachChild([&](TExprNode& sub) {
                    children.emplace_back(&sub);
                });
            } else {
                children.emplace_back(&child);
            }
        });
        return ctx.ChangeChildren(*node, std::move(children));
    }

    if (justs && size == justs + bools) {
        YQL_CLOG(DEBUG, Core) << node->Content() <<  " over Just";
        TExprNode::TListType children;
        children.reserve(size);
        node->ForEachChild([&](TExprNode& child) {
            children.emplace_back(child.IsCallable("Just") ? &child.Head() : &child);
        });
        return ctx.NewCallable(node->Pos(), "Just", {ctx.ChangeChildren(*node, std::move(children))});
    }

    if (literals) {
        YQL_CLOG(DEBUG, Core) << node->Content() <<  " over literal bools";
        TExprNode::TListType children;
        children.reserve(size);
        for (ui32 i = 0U; i < size; ++i) {
            if (node->Child(i)->IsCallable("Bool")) {
                const bool value = FromString<bool>(node->Child(i)->Head().Content());
                if (AndOr != value) {
                    return ctx.WrapByCallableIf(IsOptBoolType(*node), "Just", node->ChildPtr(i));
                }
            } else {
                children.emplace_back(node->ChildPtr(i));
            }
        }

        return children.empty() ?
            ctx.WrapByCallableIf(IsOptBoolType(*node), "Just", MakeBool(node->Pos(), AndOr, ctx)):
            ctx.ChangeChildren(*node, std::move(children));
    }

    return node;
};

TExprNode::TPtr SimplifyLogicalXor(const TExprNode::TPtr& node, TExprContext& ctx) {
    const auto size = node->ChildrenSize();
    ui32 same = 0U, justs = 0U, negations = 0U, literals = 0U, bools = 0U;
    for (ui32 i = 0U; i < size; ++i) {
        const auto child = node->Child(i);
        if (child->IsCallable("Nothing")) {
            YQL_CLOG(DEBUG, Core) << node->Content() <<  " over Nothing";
            return node->ChildPtr(i);
        }
        if (child->IsCallable(node->Content()))
            ++same;
        if (child->IsCallable("Not"))
            ++negations;
        if (child->IsCallable("Just"))
            ++justs;
        if (child->IsCallable("Bool"))
            ++literals;
        if (IsBoolType(*child))
            ++bools;
    };

    if (same) {
        YQL_CLOG(DEBUG, Core) << node->Content() <<  " over " << node->Content();
        TExprNode::TListType children;
        children.reserve(size);
        node->ForEachChild([&](TExprNode& child) {
            if (child.IsCallable(node->Content())) {
                child.ForEachChild([&](TExprNode& sub) {
                    children.emplace_back(&sub);
                });
            } else {
                children.emplace_back(&child);
            }
        });
        return ctx.ChangeChildren(*node, std::move(children));
    }

    if (justs && size == justs + bools) {
        YQL_CLOG(DEBUG, Core) << node->Content() <<  " over Just";
        TExprNode::TListType children;
        children.reserve(size);
        node->ForEachChild([&](TExprNode& child) {
            children.emplace_back(child.IsCallable("Just") ? child.HeadPtr() : &child);
        });
        return ctx.NewCallable(node->Pos(), "Just", {ctx.ChangeChildren(*node, std::move(children))});
    }

    if (literals || negations) {
        YQL_CLOG(DEBUG, Core) << node->Content() <<  " over negations or literal bools";
        TExprNode::TListType children;
        children.reserve(size);
        bool inverse = false;
        node->ForEachChild([&](TExprNode& child) {
            if (child.IsCallable("Not")) {
                children.emplace_back(child.HeadPtr());
                inverse = !inverse;
            } else if (child.IsCallable("Bool")) {
                if (FromString<bool>(child.Head().Content())) {
                    inverse = !inverse;
                }
            } else {
                children.emplace_back(&child);
            }
        });

        return children.empty() ?
            ctx.WrapByCallableIf(IsOptBoolType(*node), "Just", MakeBool(node->Pos(), inverse, ctx)):
            ctx.WrapByCallableIf(inverse, "Not", ctx.ChangeChildren(*node, std::move(children)));
    }

    return node;
};

TExprNode::TPtr SimplifyLogicalNot(const TExprNode::TPtr& node, TExprContext& ctx) {
    if (node->Head().IsCallable("Nothing")) {
        YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
        return node->HeadPtr();
    }

    if (node->Head().IsCallable("Not")) {
        YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
        return node->Head().HeadPtr();
    }

    if (node->Head().IsCallable("Just")) {
        YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
        return ctx.ChangeChild(node->Head(), 0U, ctx.ChangeChild(*node, 0U, node->Head().HeadPtr()));
    }

    if (node->Head().IsCallable("Bool")) {
        YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content() << " '" << node->Head().Head().Content();
        const auto value = FromString<bool>(node->Head().Head().Content());
        return MakeBool(node->Pos(), !value, ctx);
    }

    return node;
}

template <bool Equal>
TExprNode::TPtr OptimizeEquality(const TExprNode::TPtr& node, TExprContext& ctx) {
    if (node->Head().IsCallable("Nothing") || node->Tail().IsCallable("Nothing")) {
        YQL_CLOG(DEBUG, Core) << "Compare '" << node->Content() << "' over Nothing";
        return MakeBoolNothing(node->Pos(), ctx);
    }

    if (node->Head().IsCallable("Just")) {
        TCoJust just(node->HeadPtr());
        if (IsDataType(just.Input().Ref())) {
            YQL_CLOG(DEBUG, Core) << "Compare '" << node->Content() << "' over Just";
            auto ret = ctx.ChangeChild(*node, 0U, just.Input().Ptr());
            return ctx.WrapByCallableIf(IsDataType(node->Tail()), "Just", std::move(ret));
        }
    }

    if (node->Tail().IsCallable("Just")) {
        TCoJust just(node->TailPtr());
        if (IsDataType(just.Input().Ref())) {
            YQL_CLOG(DEBUG, Core) << "Compare '" << node->Content() << "' over Just";
            auto ret = ctx.ChangeChild(*node, 1U, just.Input().Ptr());
            return ctx.WrapByCallableIf(IsDataType(node->Head()), "Just", std::move(ret));
        }
    }

    if (IsBoolType(*node) || IsOptBoolType(*node)) {
        if (node->Head().IsCallable("Bool")) {
            YQL_CLOG(DEBUG, Core) << "Compare '" << node->Content() << "' with " << node->Head().Content() << " '" << node->Head().Head().Content();
            const auto value = FromString<bool>(node->Head().Head().Content());
            return ctx.WrapByCallableIf(Equal != value, "Not", node->TailPtr());
        }

        if (node->Tail().IsCallable("Bool")) {
            YQL_CLOG(DEBUG, Core) << "Compare '" << node->Content() << "' with " << node->Tail().Content() << " '" << node->Tail().Head().Content();
            const auto value = FromString<bool>(node->Tail().Head().Content());
            return ctx.WrapByCallableIf(Equal != value, "Not", node->HeadPtr());
        }
    }

    return node;
}

template <bool IsList, bool IsLookup = false>
TExprNode::TPtr OptimizeContains(const TExprNode::TPtr& node, TExprContext& ctx) {
    static_assert(!IsList || !IsLookup, "List or Lookup");
    if constexpr (!(IsLookup || IsList)) {
        if (IsDataOrOptionalOfData(node->Head().GetTypeAnn())) {
            return OptimizeEquality<true>(node, ctx);
        }
    }

    if (const auto& nodeToCheck = SkipCallables(node->Head(), SkippableCallables); nodeToCheck.IsCallable(IsList ? "AsList" : "AsDict")) {
        for (ui32 i = 0U; i < nodeToCheck.ChildrenSize(); ++i) {
            if ((IsList ? nodeToCheck.Child(i) : &nodeToCheck.Child(i)->Head()) == &node->Tail()) {
                YQL_CLOG(DEBUG, Core) << "Instant " << node->Content() << " in " << nodeToCheck.Content();
                return IsLookup ?
                    ctx.NewCallable(node->Pos(), "Just", {nodeToCheck.Child(i)->TailPtr()}):
                    MakeBool<true>(node->Pos(), ctx);
            }
        }
    } else if (nodeToCheck.IsCallable(IsList ? "List" : "Dict")) {
        if (1U == nodeToCheck.ChildrenSize()) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over empty " << nodeToCheck.Content();
            return IsLookup ?
                ctx.NewCallable(node->Pos(), "Nothing", {ExpandType(node->Pos(), *node->GetTypeAnn(), ctx)}):
                MakeBool<false>(node->Pos(), ctx);
        }

        for (ui32 i = 1U; i < nodeToCheck.ChildrenSize(); ++i) {
            if ((IsList ? nodeToCheck.Child(i) : &nodeToCheck.Child(i)->Head()) == &node->Tail()) {
                YQL_CLOG(DEBUG, Core) << "Instant " << node->Content() << " in " << nodeToCheck.Content();
                return IsLookup ?
                    ctx.NewCallable(node->Pos(), "Just", {nodeToCheck.Child(i)->TailPtr()}):
                    MakeBool<true>(node->Pos(), ctx);
            }
        }
    }
    return node;
}

TExprNode::TPtr OptimizeDictItems(const TExprNode::TPtr& node, TExprContext& ctx) {
    if (1U == node->Head().ChildrenSize() && node->Head().IsCallable("Dict")) {
        YQL_CLOG(DEBUG, Core) << node->Content() << " over empty " << node->Head().Content();
        return ctx.NewCallable(node->Head().Pos(), "List", {ExpandType(node->Pos(), *node->GetTypeAnn(), ctx)});
    }
    return node;
}

template <bool IsList>
TExprNode::TPtr OptimizeContainerIf(const TExprNode::TPtr& node, TExprContext& ctx) {
    if (node->Head().IsCallable("Bool")) {
        YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content() << " '" << node->Head().Head().Content();
        const auto value = FromString<bool>(node->Head().Head().Content());
        auto res = value
            ? ctx.NewCallable(node->Tail().Pos(), IsList ? "AsList" : "Just", {node->TailPtr()})
            : //TODO: ctx.NewCallable(node->Head().Pos(), IsList ? "List" : "Nothing", {ExpandType(node->Pos(), *node->GetTypeAnn(), ctx)})
              ctx.Builder(node->Head().Pos())
                .Callable(IsList ? "List" : "Nothing")
                    .Callable(0, IsList ? "ListType" : "OptionalType")
                        .Callable(0, "TypeOf")
                            .Add(0, node->TailPtr())
                        .Seal()
                    .Seal()
                .Seal().Build();
        if (IsList) {
            res = KeepConstraints(res, *node, ctx);
        }
        return res;

    }
    return node;
}

template <bool IsList>
TExprNode::TPtr OptimizeFlatContainerIf(const TExprNode::TPtr& node, TExprContext& ctx) {
    if (IsPredicateFlatMap(node->Tail())) {
        YQL_CLOG(DEBUG, Core) << "Fuse " << node->Content() << " with " << node->Tail().Content() << " '" << node->Head().Head().Content();
        return ctx.Builder(node->Pos())
            .Callable(node->Tail().Content())
                .Callable(0, "And")
                    .Add(0, node->HeadPtr())
                    .Add(1, node->Tail().HeadPtr())
                .Seal()
                .Add(1, node->Tail().TailPtr())
            .Seal().Build();
    }

    const auto& nodeToCheck = SkipCallables(node->Tail(), SkippableCallables);
    if (1U == nodeToCheck.ChildrenSize() && nodeToCheck.IsCallable(IsList ? "AsList" : "Just")) {
        YQL_CLOG(DEBUG, Core) << node->Content() << " with " << nodeToCheck.Content();
        auto res = ctx.NewCallable(node->Pos(), IsList ? "ListIf" : "OptionalIf", {node->HeadPtr(), nodeToCheck.HeadPtr()});
        if (IsList) {
            res = KeepSortedConstraint(res, node->GetConstraint<TSortedConstraintNode>(), ctx);
        }
        return res;
    }

    if (1U == nodeToCheck.ChildrenSize() && nodeToCheck.IsCallable(IsList ? "List" : "Nothing")) {
        YQL_CLOG(DEBUG, Core) << node->Content() << " with " << nodeToCheck.Content();
        auto res = node->TailPtr();
    }

    if (node->Head().IsCallable("Bool")) {
        YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content() << " '" << node->Head().Head().Content();
        const auto value = FromString<bool>(node->Head().Head().Content());
        return value
            ? node->TailPtr()
            : KeepConstraints(
                ctx.NewCallable(node->Head().Pos(), IsList ? "List" : "Nothing", {ExpandType(node->Tail().Pos(), *node->GetTypeAnn(), ctx)}),
                *node,
                ctx);
    }

    return node;
}

template <bool HeadOrTail>
TExprNode::TPtr OptimizeToOptional(const TExprNode::TPtr& node, TExprContext& ctx) {
    if (node->Head().IsCallable("ToList")) {
        YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
        return node->Head().HeadPtr();
    }

    const auto& nodeToCheck = SkipCallables(node->Head(), SkippableCallables);
    if (nodeToCheck.IsCallable("AsList")) {
        YQL_CLOG(DEBUG, Core) << node->Content() << " over " << nodeToCheck.Content();
        return ctx.NewCallable(node->Head().Pos(), "Just", {HeadOrTail ? nodeToCheck.HeadPtr() : nodeToCheck.TailPtr()});
    }

    if (1U == nodeToCheck.ChildrenSize() && nodeToCheck.IsCallable("List")) {
        YQL_CLOG(DEBUG, Core) << node->Content() << " over empty " << nodeToCheck.Content();
        return ctx.NewCallable(node->Head().Pos(), "Nothing", {ExpandType(node->Pos(), *node->GetTypeAnn(), ctx)});
    }

    return node;
}

TExprNode::TPtr ExtractMember(const TExprNode& node) {
    auto memberName = node.Tail().Content();
    for (ui32 index = 0; index < node.Head().ChildrenSize(); ++index) {
        auto tuple = node.Head().Child(index);
        if (tuple->Head().Content() == memberName) {
            return tuple->TailPtr();
        }
    }

    YQL_ENSURE(false, "Unexpected member name: " << memberName);
}

template <bool RightOrLeft>
TExprNode::TPtr OptimizeDirection(const TExprNode::TPtr& node) {
    if (node->Head().IsCallable(ConsName)) {
        YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
        return RightOrLeft ? node->Head().TailPtr() : node->Head().HeadPtr();
    }
    return node;
}

TExprNode::TPtr OptimizeAsStruct(const TExprNode::TPtr& node, TExprContext& ctx) {
    TExprNode::TPtr singleFrom;
    for (const auto& member : node->Children()) {
        if (!member->Child(1)->IsCallable("Member")) {
            return node;
        }

        if (member->Head().Content() != member->Child(1)->Child(1)->Content()) {
            return node;
        }

        auto from = member->Child(1)->HeadPtr();
        if (!singleFrom) {
            if (from->GetTypeAnn()->GetKind() != ETypeAnnotationKind::Struct) {
                return node;
            }

            singleFrom = from;
        } else {
            if (singleFrom != from) {
                return node;
            }
        }
    }

    if (!singleFrom) {
        return node;
    }

    if (singleFrom->GetTypeAnn()->Cast<TStructExprType>()->GetSize() == node->ChildrenSize()) {
        YQL_CLOG(DEBUG, Core) << "CheckClonedStructure";
        return singleFrom;
    }

    if (TCoVisit::Match(singleFrom.Get())) {
        YQL_CLOG(DEBUG, Core) << node->Content() << " over " << singleFrom->Content();
        return ctx.Builder(node->Pos())
            .Callable("Visit")
                .Add(0, singleFrom->HeadPtr())
                .Do([&](TExprNodeBuilder& parent) -> TExprNodeBuilder& {
                    for (size_t i = 1; i < singleFrom->ChildrenSize(); ++i) {
                        auto child = singleFrom->ChildPtr(i);
                        if (child->IsAtom()) {
                            auto lambda = singleFrom->Child(i + 1);
                            parent
                                .Add(i, std::move(child))
                                .Lambda(i + 1)
                                    .Param("visitItem")
                                    .ApplyPartial(lambda->HeadPtr(), node)
                                        .WithNode(*singleFrom, lambda->TailPtr())
                                        .With(0, "visitItem")
                                    .Seal()
                                .Seal();
                            ++i;
                        }
                        else {
                            parent.ApplyPartial(i, {}, node)
                                .WithNode(*singleFrom, std::move(child))
                            .Seal();
                        }
                    }
                    return parent;
                })
            .Seal()
            .Build();
    }

    return node;
}

TExprNode::TPtr RemoveToStringFromString(const TExprNode::TPtr& node) {
    if (node->Head().GetTypeAnn()->GetKind() == ETypeAnnotationKind::Data &&
        node->Head().GetTypeAnn()->Cast<TDataExprType>()->GetSlot() == EDataSlot::String) {
          YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
          return node->HeadPtr();
    }

    return node;
}

template <typename TMapType, typename TFlatMapType>
TExprNode::TPtr ConvertMapToFlatmap(TMapType map, TExprContext& ctx) {
    auto list = map.Input();
    auto lambda = map.Lambda();

    auto ret = Build<TFlatMapType>(ctx, map.Pos())
            .Input(list)
            .Lambda()
                .Args({ "item" })
                .template Body<TCoJust>()
                    .template Input<TExprApplier>()
                        .Apply(lambda)
                        .With(0, "item")
                        .Build()
                    .Build()
                .Build()
            .Done();

    return ret.Ptr();
}

template <typename TFilterType, typename TFlatMapType>
TExprNode::TPtr ConvertFilterToFlatmap(TFilterType filter, TExprContext& ctx, TOptimizeContext& optCtx) {
    const auto& list = filter.Input();
    const auto& lambda = filter.Lambda();

    if (const auto& limit = filter.Limit()) {
        const auto ret = Build<TCoTake>(ctx, filter.Pos())
                .template Input<TFilterType>()
                    .Input(list)
                    .Lambda(lambda)
                .Build()
                .Count(limit.Cast())
            .Done();
        return ret.Ptr();
    }

    const auto ret = Build<TFlatMapType>(ctx, filter.Pos())
            .Input(list)
            .Lambda()
                .Args({ "item" })
                .template Body<TCoOptionalIf>()
                    .template Predicate<TExprApplier>()
                        .Apply(lambda)
                        .With(0, "item")
                        .Build()
                    .Value("item")
                    .Build()
                .Build()
            .Done();
    return KeepColumnOrder(ret.Ptr(), filter.Ref(), ctx, *optCtx.Types);
}

TExprNode::TPtr ExtractPredicateFromFlatmapOverListIf(const TExprNode& node, TExprContext& ctx) {
    const bool isOptional = node.Tail().GetTypeAnn()->GetKind() == ETypeAnnotationKind::Optional;
    const bool needWrap = !isOptional && node.Tail().GetTypeAnn()->GetKind() != ETypeAnnotationKind::List;

    auto item = ctx.ReplaceNode(node.Tail().TailPtr(), node.Tail().Head().Head(), node.Head().TailPtr());
    item = ctx.WrapByCallableIf(needWrap, "ForwardList", std::move(item));

    auto ret = ctx.NewCallable(node.Head().Pos(), isOptional ? "FlatOptionalIf" : "FlatListIf",
        { node.Head().HeadPtr(), std::move(item) });

    if (isOptional && node.GetTypeAnn()->GetKind() == ETypeAnnotationKind::List) {
        ret = ctx.NewCallable(node.Head().Pos(), "ToList", { std::move(ret) });
    } else if (node.GetTypeAnn()->GetKind() == ETypeAnnotationKind::Flow) {
        ret = ctx.NewCallable(node.Head().Pos(), "ToFlow", { std::move(ret) });
    } else if (node.GetTypeAnn()->GetKind() == ETypeAnnotationKind::Stream) {
        ret = ctx.NewCallable(node.Head().Pos(), "ToStream", { std::move(ret) });
    }

    return ret;
}

TExprNode::TPtr ExtractPredicateFromFlatmapOverFlatListIf(const TExprNode& node, TExprContext& ctx) {
    auto newFlatMap = ctx.ChangeChild(node, 0U, node.Head().TailPtr());
    return ctx.NewCallable(node.Head().Pos(),
        node.GetTypeAnn()->GetKind() == ETypeAnnotationKind::List ?
            "FlatListIf" : node.Head().Content(), {
            node.Head().HeadPtr(),
            std::move(newFlatMap)
        });
}

TExprNode::TPtr FuseJustOrSingleAsListWithFlatmap(const TExprNode::TPtr& node, TExprContext& ctx) {
    // input    F L S O
    // lambda L F L S L
    // lambda S F L S S
    // lambda O F L S O
    // lambda F F F - F
    TCoFlatMapBase self(node);
    const auto inputItem = self.Input().Ref().HeadPtr();
    auto result = ctx.ReplaceNode(self.Lambda().Body().Ptr(), self.Lambda().Args().Arg(0).Ref(), inputItem);
    if (self.Input().Maybe<TCoJust>()) {
        // output type is the same as lambda return type
        return result;
    }

    const auto lambdaReturnKind = self.Lambda().Ref().GetTypeAnn()->GetKind();
    switch (lambdaReturnKind) {
        case ETypeAnnotationKind::List:
        case ETypeAnnotationKind::Flow:
            // output type is the same as lambda return type
            break;
        case ETypeAnnotationKind::Optional:
            result = ctx.NewCallable(result->Pos(), "ToList", { result });
            break;
        default:
            YQL_ENSURE(lambdaReturnKind == ETypeAnnotationKind::Stream);
            // we can safely use ForwardList here since lambda can not yield
            result = ctx.NewCallable(result->Pos(), "ForwardList", { result });
    }

    return result;
}

TExprNode::TPtr FuseToListWithFlatmap(const TExprNode::TPtr& node, TExprContext& ctx) {
    TCoFlatMapBase self(node);
    const auto inputItem = self.Input().Ref().HeadPtr();
    YQL_ENSURE(inputItem->GetTypeAnn()->GetKind() == ETypeAnnotationKind::Optional);

    const auto lambdaReturnKind = self.Lambda().Ref().GetTypeAnn()->GetKind();
    auto result = ctx.ChangeChild(*node, 0U, TExprNode::TPtr(inputItem));
    if (lambdaReturnKind == ETypeAnnotationKind::Optional) {
        result = ctx.NewCallable(result->Pos(), "ToList", { result });
    } else if (lambdaReturnKind == ETypeAnnotationKind::Stream) {
        result = ctx.NewCallable(result->Pos(), "ForwardList", { result });
    }
    return result;
}

bool ShouldConvertSqlInToJoin(const TCoSqlIn& sqlIn, bool /* negated */) {
    bool tableSource = false;

    for (const auto& hint : sqlIn.Options()) {
        if (hint.Name().Value() == TStringBuf("isCompact")) {
            return false;
        }
        if (hint.Name().Value() == TStringBuf("tableSource")) {
            tableSource = true;
        }
    }

    return tableSource;
}

bool CanConvertSqlInToJoin(const TCoSqlIn& sqlIn) {
    auto leftArg = sqlIn.Lookup();
    auto leftColumnType = leftArg.Ref().GetTypeAnn();

    auto rightArg = sqlIn.Collection();
    auto rightArgType = rightArg.Ref().GetTypeAnn();

    if (rightArgType->GetKind() == ETypeAnnotationKind::List) {
        auto rightListItemType = rightArgType->Cast<TListExprType>()->GetItemType();

        auto isDataOrTupleOfData = [](const TTypeAnnotationNode* type) {
            if (IsDataOrOptionalOfData(type)) {
                return true;
            }
            if (type->GetKind() == ETypeAnnotationKind::Tuple) {
                return AllOf(type->Cast<TTupleExprType>()->GetItems(), [](const auto& item) {
                    return IsDataOrOptionalOfData(item);
                });
            }
            return false;
        };

        if (rightListItemType->GetKind() == ETypeAnnotationKind::Struct) {
            auto rightStructType = rightListItemType->Cast<TStructExprType>();
            YQL_ENSURE(rightStructType->GetSize() == 1);
            auto rightColumnType = rightStructType->GetItems()[0]->GetItemType();
            return isDataOrTupleOfData(rightColumnType);
        }

        return isDataOrTupleOfData(rightListItemType);
    }

    /**
     * todo: support tuple of equal tuples
     *
     * sql expression \code{.sql} ... where (k1, k2) in ((1, 2), (2, 3), (3, 4)) \endcode
     * is equivalent to the \code{.sql} ... where (k1, k2) in AsTuple((1, 2), (2, 3), (3, 4)) \endcode
     * but not to the \code{.sql} ... where (k1, k2) in AsList((1, 2), (2, 3), (3, 4)) \endcode
     * so, it's not supported now
     */

    if (rightArgType->GetKind() == ETypeAnnotationKind::Dict) {
        auto rightDictType = rightArgType->Cast<TDictExprType>()->GetKeyType();
        return IsDataOrOptionalOfData(leftColumnType) && IsDataOrOptionalOfData(rightDictType);
    }

    return false;
}

struct TPredicateChainNode {
    TExprNode::TPtr Pred;

    bool Negated = false;
    bool ConvertibleToJoin = false;

    // extra predicates due to NOT IN + nulls
    TExprNode::TPtr ExtraLeftPred;
    TExprNode::TPtr ExtraRightPred;

    // SqlIn params
    TPositionHandle SqlInPos;
    TExprNode::TPtr Left; // used only if LeftArgColumns is empty
    TExprNode::TPtr Right;

    TVector<TStringBuf> LeftArgColumns;  // set if left side of IN is input column reference or tuple of columns references
    TVector<TString> RightArgColumns; // always set
};

using TPredicateChain = TVector<TPredicateChainNode>;

void SplitSqlInCollection(const TCoSqlIn& sqlIn, TExprNode::TPtr& collectionNoNulls,
    TExprNode::TPtr& collectionNulls, TExprContext& ctx)
{
    auto collection = sqlIn.Collection().Ptr();
    const bool isTableSource = HasSetting(sqlIn.Options().Ref(), "tableSource");

    auto collectionItemExtractorLambda = ctx.Builder(collection->Pos())
        .Lambda()
            .Param("listItem")
            .Arg("listItem")
        .Seal()
        .Build();

    TExprNode::TPtr collectionAsList = collection;
    auto collectionKind = collection->GetTypeAnn()->GetKind();
    if (collectionKind == ETypeAnnotationKind::Dict) {
        collectionAsList = ctx.Builder(collection->Pos())
            .Callable("DictKeys")
                .Add(0, collectionAsList)
            .Seal()
            .Build();
    } else {
        YQL_ENSURE(collectionKind == ETypeAnnotationKind::List,
            "Unexpected collection type: " << *collection->GetTypeAnn());
        if (isTableSource) {
            auto listItemType = collection->GetTypeAnn()->Cast<TListExprType>()->GetItemType();
            YQL_ENSURE(listItemType->GetKind() == ETypeAnnotationKind::Struct);

            auto structType = listItemType->Cast<TStructExprType>();
            YQL_ENSURE(structType->GetSize() == 1);
            TStringBuf memberName = structType->GetItems()[0]->GetName();

            collectionItemExtractorLambda = ctx.Builder(collection->Pos())
                .Lambda()
                    .Param("listItem")
                    .Callable("Member")
                        .Arg(0, "listItem")
                        .Atom(1, memberName)
                    .Seal()
                .Seal()
                .Build();
        }
    }

    auto buildFilter = [&](bool nulls) {
        return ctx.Builder(collection->Pos())
            .Callable("OrderedFilter")
                .Add(0, collectionAsList)
                .Lambda(1)
                    .Param("listItem")
                    .Callable("If")
                        .Callable(0, "Exists")
                            .Apply(0, collectionItemExtractorLambda)
                                .With(0, "listItem")
                            .Seal()
                        .Seal()
                        .Add(1, MakeBool(collection->Pos(), !nulls, ctx))
                        .Add(2, MakeBool(collection->Pos(),  nulls, ctx))
                    .Seal()
                .Seal()
            .Seal()
            .Build();
    };

    collectionNoNulls = buildFilter(false);
    collectionNulls = buildFilter(true);
}

TExprNode::TPtr BuildCollectionEmptyPred(TPositionHandle pos, const TExprNode::TPtr& collectionAsList, TExprContext& ctx) {
    return ctx.Builder(pos)
        .Callable("Not")
            .Callable(0, "HasItems")
                .Callable(0, "Take")
                    .Add(0, collectionAsList)
                    .Callable(1, "Uint64")
                        .Atom(0, "1", TNodeFlags::Default)
                    .Seal()
                .Seal()
            .Seal()
        .Seal()
        .Build();
}

TExprNode::TPtr BuildSqlInCollectionEmptyPred(const TCoSqlIn& sqlIn, TExprContext& ctx) {
    auto collection = sqlIn.Collection().Ptr();
    const auto collectionType = sqlIn.Collection().Ref().GetTypeAnn();

    TExprNode::TPtr collectionEmptyPred;
    switch (collectionType->GetKind()) {
        case ETypeAnnotationKind::Tuple:
            collectionEmptyPred = MakeBool(sqlIn.Pos(), collectionType->Cast<TTupleExprType>()->GetSize() == 0, ctx);
            break;
        case ETypeAnnotationKind::Dict:
            collection = ctx.Builder(sqlIn.Pos())
                .Callable("DictKeys")
                    .Add(0, collection)
                .Seal()
                .Build();
            [[fallthrough]];
        case ETypeAnnotationKind::List:
            collectionEmptyPred = BuildCollectionEmptyPred(sqlIn.Pos(), collection, ctx);
            break;
        default:
            YQL_ENSURE(false, "Unexpected collection type: " << *collectionType);
    }
    return collectionEmptyPred;
}

TPredicateChainNode ParsePredicateChainNode(const TExprNode::TPtr& predicate, const TExprNode::TPtr& topLambdaArg,
    std::function<bool(const TCoSqlIn&, bool /* negated */)> shouldConvertSqlInToJoin, TExprContext& ctx)
{
    TPredicateChainNode result;

    result.Pred = predicate;

    auto curr = predicate;
    TExprNode::TPtr pred;
    if (curr->IsCallable("Not")) {
        curr = curr->HeadPtr();
        result.Negated = true;
    }

    TExprNode::TPtr leftArg;
    bool hasCoalesce = false;
    if (curr->IsCallable("SqlIn")) {
        leftArg = curr->ChildPtr(1);
    } else if (curr->IsCallable("Coalesce") &&
               curr->Head().IsCallable("SqlIn") &&
               curr->Child(1)->IsCallable("Bool")) {
        bool coalesceVal = FromString<bool>(curr->Child(1)->Head().Content());
        if (coalesceVal == result.Negated) {
            curr = curr->HeadPtr();
            leftArg = curr->ChildPtr(1);
        }
        hasCoalesce = true;
    }

    if (!leftArg) {
        // not SqlIn
        return result;
    }

    TCoSqlIn sqlIn(curr);
    if (!shouldConvertSqlInToJoin(sqlIn, result.Negated) || !CanConvertSqlInToJoin(sqlIn)) {
        // not convertible to join
        return result;
    }

    result.SqlInPos = sqlIn.Pos();
    result.ConvertibleToJoin = true;
    result.Left = leftArg;

    if (result.Negated && HasSetting(sqlIn.Options().Ref(), "ansi")) {
        const bool nullsProcessed = HasSetting(sqlIn.Options().Ref(), "nullsProcessed");
        const bool lookupIsOptional = sqlIn.Lookup().Ref().GetTypeAnn()->GetKind() == ETypeAnnotationKind::Optional;
        const bool collectionItemsNullable = IsSqlInCollectionItemsNullable(sqlIn);
        if (!nullsProcessed && (collectionItemsNullable || lookupIsOptional)) {
            YQL_ENSURE(sqlIn.Ref().GetTypeAnn()->GetKind() == ETypeAnnotationKind::Optional);
            YQL_ENSURE(hasCoalesce);

            // need to add nullsProcessed setting
            result.Pred = nullptr;

            auto rebuildMainPred = [&sqlIn, &ctx](const TExprNode::TPtr& collection) {
                return ctx.Builder(sqlIn.Pos())
                    .Callable("Not")
                        .Callable(0, "Coalesce")
                            .Callable(0, "SqlIn")
                                .Add(0, collection)
                                .Add(1, sqlIn.Lookup().Ptr())
                                .Add(2, AddSetting(sqlIn.Options().Ref(), sqlIn.Options().Pos(), "nullsProcessed", nullptr, ctx))
                            .Seal()
                            .Add(1, MakeBool(sqlIn.Pos(), true, ctx))
                        .Seal()
                    .Seal()
                    .Build();
            };

            if (collectionItemsNullable) {
                TExprNode::TPtr collectionNoNulls;
                TExprNode::TPtr collectionNulls;
                SplitSqlInCollection(sqlIn, collectionNoNulls, collectionNulls, ctx);

                result.ExtraRightPred = BuildCollectionEmptyPred(sqlIn.Pos(), collectionNulls, ctx);
                result.Pred = rebuildMainPred(collectionNoNulls);
            }

            if (lookupIsOptional) {
                result.ExtraLeftPred = ctx.Builder(sqlIn.Pos())
                    .Callable("Or")
                        .Callable(0, "Exists")
                            .Add(0, sqlIn.Lookup().Ptr())
                        .Seal()
                        .Add(1, BuildSqlInCollectionEmptyPred(sqlIn, ctx))
                    .Seal()
                    .Build();
            }

            if (!result.Pred) {
                result.Pred = rebuildMainPred(sqlIn.Collection().Ptr());
            }

            return result;
        }
    }

    auto isMemberOf = [](const TExprNode::TPtr& node, const TExprNode::TPtr& arg) {
        return node->IsCallable("Member") && node->HeadPtr() == arg;
    };

    if (isMemberOf(leftArg, topLambdaArg)) {
        // left side of IN is column reference
        result.LeftArgColumns.emplace_back(leftArg->Child(1)->Content());
    } else if (leftArg->GetTypeAnn()->GetKind() == ETypeAnnotationKind::Tuple) {
        // if leftArg is tuple of members then replace tuple with its members
        for (const auto& tupleItem : leftArg->Children()) {
            if (isMemberOf(tupleItem, topLambdaArg)) {
                result.LeftArgColumns.emplace_back(tupleItem->Child(1)->Content());
            } else {
                // fallback to join on whole tuple
                result.LeftArgColumns.clear();
                break;
            }
        }
    }

    auto rightArg = sqlIn.Collection().Ptr();
    auto rightArgType = rightArg->GetTypeAnn();

    if (rightArgType->GetKind() == ETypeAnnotationKind::List) {
        auto rightArgItemType = rightArgType->Cast<TListExprType>()->GetItemType();

        if (rightArgItemType->GetKind() == ETypeAnnotationKind::Struct) {
            auto rightStructType = rightArgItemType->Cast<TStructExprType>();
            YQL_ENSURE(rightStructType->GetSize() == 1);

            const TItemExprType* itemType = rightStructType->GetItems()[0];
            if (IsDataOrOptionalOfData(itemType->GetItemType())) {
                result.Right = rightArg;
                result.RightArgColumns = { ToString(itemType->GetName()) };
                return result;
            }

            YQL_ENSURE(itemType->GetItemType()->GetKind() == ETypeAnnotationKind::Tuple);

            rightArg = Build<TCoFlatMap>(ctx, rightArg->Pos())
                    .Input(rightArg)
                    .Lambda()
                        .Args({"item"})
                        .Body<TCoJust>()
                            .Input<TCoMember>()
                                .Struct("item")
                                .Name().Build(itemType->GetName())
                                .Build()
                            .Build()
                        .Build()
                    .Done()
                    .Ptr();

            if (!result.LeftArgColumns.empty()) {
                auto rowArg = Build<TCoArgument>(ctx, sqlIn.Pos())
                        .Name("row")
                        .Done();
                auto asStructBuilder = Build<TCoAsStruct>(ctx, sqlIn.Pos());
                for (size_t i = 0; i < itemType->GetItemType()->Cast<TTupleExprType>()->GetItems().size(); ++i) {
                    const TString columnName = TStringBuilder() << "_yql_sqlin_tuple_" << i;
                    asStructBuilder.Add<TCoNameValueTuple>()
                            .Name().Build(columnName)
                            .Value<TCoNth>()
                                .Tuple(rowArg)
                                .Index(ctx.NewAtom(sqlIn.Pos(), ToString(i)))
                                .Build()
                            .Build();
                    result.RightArgColumns.emplace_back(columnName);
                }
                result.Right = Build<TCoMap>(ctx, sqlIn.Pos())
                        .Input(rightArg)
                        .Lambda()
                            .Args(rowArg)
                            .Body(asStructBuilder.Done())
                            .Build()
                        .Done()
                        .Ptr();

                return result;
            }

            // fallthrough to default join by the whole tuple
        } else if (rightArgItemType->GetKind() == ETypeAnnotationKind::Tuple) {
            auto tupleItemTypes = rightArgItemType->Cast<TTupleExprType>()->GetItems();

            if (!result.LeftArgColumns.empty()) {
                auto rowArg = Build<TCoArgument>(ctx, sqlIn.Pos())
                        .Name("row")
                        .Done();
                auto asStructBuilder = Build<TCoAsStruct>(ctx, sqlIn.Pos());
                for (size_t i = 0; i < tupleItemTypes.size(); ++i) {
                    const TString columnName = TStringBuilder() << "_yql_sqlin_tuple_" << i;
                    asStructBuilder.Add<TCoNameValueTuple>()
                            .Name().Build(columnName)
                            .Value<TCoNth>()
                                .Tuple(rowArg)
                                .Index(ctx.NewAtom(sqlIn.Pos(), ToString(i)))
                                .Build()
                            .Build();
                    result.RightArgColumns.emplace_back(columnName);
                }
                result.Right = Build<TCoMap>(ctx, sqlIn.Pos())
                        .Input(rightArg)
                        .Lambda()
                            .Args(rowArg)
                            .Body(asStructBuilder.Done())
                            .Build()
                        .Done()
                        .Ptr();
                return result;
            }

            // fallthrough to default join by the whole tuple
        } else {
            YQL_ENSURE(IsDataOrOptionalOfData(rightArgItemType), "" << FormatType(rightArgItemType));
        }

        // rewrite List<DataType|Tuple> to List<Struct<key: DataType|Tuple>>
        result.Right = Build<TCoMap>(ctx, sqlIn.Pos())
                .Input(rightArg)
                .Lambda()
                    .Args({"item"})
                    .Body<TCoAsStruct>()
                        .Add<TCoNameValueTuple>()
                            .Name().Build("key")
                            .Value("item")
                            .Build()
                        .Build()
                    .Build()
                .Done()
                .Ptr();
        result.RightArgColumns = { "key" };

        return result;
    }

    YQL_ENSURE(rightArgType->GetKind() == ETypeAnnotationKind::Dict, "" << FormatType(rightArgType));

    auto rightDictType = rightArgType->Cast<TDictExprType>()->GetKeyType();
    YQL_ENSURE(IsDataOrOptionalOfData(rightDictType));

    auto dictKeys = ctx.Builder(sqlIn.Pos())
        .Callable("DictKeys")
            .Add(0, rightArg)
        .Seal()
        .Build();

    result.Right = Build<TCoMap>(ctx, sqlIn.Pos())
            .Input(dictKeys)
            .Lambda()
                .Args({"item"})
                .Body<TCoAsStruct>()
                    .Add<TCoNameValueTuple>()
                        .Name().Build("key")
                        .Value("item")
                        .Build()
                    .Build()
                .Build()
            .Done()
            .Ptr();
    result.RightArgColumns = { "key" };

    return result;
}

TExprNode::TPtr SplitPredicateChain(TExprNode::TPtr&& node, const TExprNode::TPtr& topLambdaArg,
    std::function<bool(const TCoSqlIn&, bool /* negated */)> shouldConvertSqlInToJoin, TPredicateChain& prefix,
    TExprContext& ctx)
{
    if (!node->IsCallable("And")) {
        TPredicateChainNode curr = ParsePredicateChainNode(node, topLambdaArg, shouldConvertSqlInToJoin, ctx);
        if (!prefix.empty() && prefix.back().ConvertibleToJoin != curr.ConvertibleToJoin) {
            // stop splitting
            return std::move(node);
        }

        prefix.emplace_back(curr);
        return {};
    }

    auto children = node->ChildrenList();

    for (auto& child : children) {
        child = SplitPredicateChain(std::move(child), topLambdaArg, shouldConvertSqlInToJoin, prefix, ctx);
        if (child) {
            break;
        }
    }

    if (children.front().Get() == &node->Head()) {
        return std::move(node);
    }

    children.erase(std::remove_if(children.begin(), children.end(), std::logical_not<TExprNode::TPtr>()), children.end());

    if (children.empty()) {
        return {};
    }
    return 1U == children.size() ? std::move(children.front()) : ctx.ChangeChildren(*node, std::move(children));
}

TExprNode::TPtr RebuildFlatmapOverPartOfPredicate(const TExprNode::TPtr& origFlatMap, const TExprNode::TPtr& input,
                                                  const TExprNode::TPtr& pred, bool isOuter, TExprContext& ctx)
{
    auto origLambdaArgs = origFlatMap->Child(1)->HeadPtr();
    TCoConditionalValueBase origConditional(origFlatMap->Child(1)->TailPtr());
    auto newLambdaBody = isOuter ?
        ctx.ChangeChild(origConditional.Ref(), TCoConditionalValueBase::idx_Predicate, TExprNode::TPtr(pred)) :
        ctx.NewCallable(origFlatMap->Pos(), "OptionalIf", {pred, origLambdaArgs->HeadPtr()});

    bool isOrdered = origFlatMap->IsCallable({"OrderedFlatMap", "OrderedFlatMapToEquiJoin"});
    auto resultingName = isOrdered ? "OrderedFlatMap" : "FlatMap";

    return ctx.Builder(origFlatMap->Pos())
        .Callable(resultingName)
            .Add(0, input)
            .Lambda(1)
                .Param("item")
                .ApplyPartial(origLambdaArgs, newLambdaBody)
                    .With(0, "item")
                .Seal()
            .Seal()
        .Seal()
        .Build();
}

TExprNode::TPtr BuildEquiJoinForSqlInChain(const TExprNode::TPtr& flatMapNode, const TPredicateChain& chain, TExprContext& ctx) {
    YQL_ENSURE(!chain.empty());

    auto input = flatMapNode->HeadPtr();
    bool isOrdered = flatMapNode->IsCallable({"OrderedFlatMap", "OrderedFlatMapToEquiJoin"});
    auto origLambdaArgs = flatMapNode->Child(1)->HeadPtr();

    // placeholder for input table
    TExprNode::TListType equiJoinArgs(1);
    equiJoinArgs.reserve(chain.size() + 3);

    TExprNode::TPtr joinChain;
    TExprNode::TPtr addMemberChain;
    TExprNode::TListType renames;

    static const TStringBuf inputTable = "_yql_injoin_input";
    auto inputTableAtom = ctx.NewAtom(input->Pos(), inputTable);

    for (size_t i = 0; i < chain.size(); ++i) {
        const TString tableName = TStringBuilder() << "_yql_injoin_" << i;
        const TString columnName = TStringBuilder() << "_yql_injoin_column_" << i;
        const auto pos = chain[i].SqlInPos;

        auto equiJoinArg = ctx.Builder(pos)
            .List()
                .Add(0, chain[i].Right)
                .Atom(1, tableName)
            .Seal()
            .Build();

        equiJoinArgs.push_back(equiJoinArg);

        TExprNodeList leftKeys;
        if (chain[i].LeftArgColumns.empty()) {
            leftKeys.push_back(inputTableAtom);
            leftKeys.push_back(ctx.NewAtom(pos, columnName));
        } else {
            for (TStringBuf leftKey : chain[i].LeftArgColumns) {
                leftKeys.push_back(inputTableAtom);
                leftKeys.push_back(ctx.NewAtom(pos, leftKey));
            }
        }

        TExprNodeList rightKeys;
        for (const TString& rightKey : chain[i].RightArgColumns) {
            rightKeys.push_back(ctx.NewAtom(pos, tableName));
            rightKeys.push_back(ctx.NewAtom(pos, rightKey));
        }

        joinChain = ctx.Builder(pos)
            .List()
                .Atom(0, chain[i].Negated ? "LeftOnly" : "LeftSemi")
                .Add(1, joinChain ? joinChain : inputTableAtom)
                .Atom(2, tableName)
                .List(3)
                    .Add(std::move(leftKeys))
                .Seal()
                .List(4)
                    .Add(std::move(rightKeys))
                .Seal()
                .List(5)
                .Seal()
            .Seal()
            .Build();

        if (chain[i].LeftArgColumns.empty()) {
            auto rename = ctx.Builder(pos)
                .List()
                    .Atom(0, "rename")
                    .Atom(1, FullColumnName(inputTable, columnName))
                    .Atom(2, "")
                .Seal()
                .Build();
            renames.push_back(rename);

            addMemberChain = ctx.Builder(chain[i].SqlInPos)
                .Callable("AddMember")
                    .Add(0, addMemberChain ? addMemberChain : origLambdaArgs->HeadPtr())
                    .Atom(1, columnName)
                    .Add(2, chain[i].Left)
                .Seal()
                .Build();
        }
    }

    YQL_ENSURE(input->GetTypeAnn()->GetKind() == ETypeAnnotationKind::List);
    auto inputRowType = input->GetTypeAnn()->Cast<TListExprType>()->GetItemType();
    YQL_ENSURE(inputRowType->GetKind() == ETypeAnnotationKind::Struct);

    for (const auto& i : inputRowType->Cast<TStructExprType>()->GetItems()) {
        auto rename = ctx.Builder(input->Pos())
            .List()
                .Atom(0, "rename")
                .Atom(1, FullColumnName(inputTable, i->GetName()))
                .Atom(2, i->GetName())
            .Seal()
            .Build();
        renames.push_back(rename);
    }
    renames.push_back(ctx.Builder(input->Pos())
        .List()
            .Atom(0, "keep_sys")
        .Seal()
        .Build());

    equiJoinArgs.push_back(joinChain);
    equiJoinArgs.push_back(ctx.NewList(input->Pos(), std::move(renames)));

    if (addMemberChain) {
        input = ctx.Builder(input->Pos())
            .Callable(isOrdered ? "OrderedMap" : "Map")
                .Add(0, input)
                .Lambda(1)
                    .Param("item")
                    .ApplyPartial(origLambdaArgs, addMemberChain)
                        .With(0, "item")
                    .Seal()
                .Seal()
            .Seal()
            .Build();
    }

    equiJoinArgs[0] = ctx.Builder(input->Pos())
        .List()
            .Add(0, input)
            .Add(1, inputTableAtom)
        .Seal()
        .Build();

    return ctx.NewCallable(input->Pos(), "EquiJoin", std::move(equiJoinArgs));
}

TStringBuf GetEmptyCollectionName(ETypeAnnotationKind kind) {
    switch (kind) {
        case ETypeAnnotationKind::Flow:
        case ETypeAnnotationKind::Stream:   return "EmptyIterator";
        case ETypeAnnotationKind::List:     return "List";
        case ETypeAnnotationKind::Optional: return "Nothing";
        case ETypeAnnotationKind::Dict:     return "Dict";
        default: break;
    }
    return {};
}

TStringBuf GetEmptyCollectionName(const TTypeAnnotationNode* type) {
    return GetEmptyCollectionName(type->GetKind());
}

template <bool Ordered>
TExprNode::TPtr SimpleFlatMap(const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) {
    const TCoFlatMapBase self(node);
    const auto& lambdaBody = self.Lambda().Body().Ref();
    const auto& lambdaArg = self.Lambda().Args().Arg(0).Ref();

    if (!Ordered && IsListReorder(node->Head())) {
        YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
        return ctx.ChangeChild(*node, 0U, node->Head().HeadPtr());
    }

    if (node->Head().IsCallable({"ListIf", "OptionalIf"})) {
        YQL_CLOG(DEBUG, Core) << "Extract predicate from " << node->Content() << " over " << node->Head().Content();
        return ExtractPredicateFromFlatmapOverListIf(*node, ctx);
    }

    if (node->Head().IsCallable({"FlatListIf", "FlatOptionalIf"})) {
        YQL_CLOG(DEBUG, Core) << "Extract predicate from " << node->Content() << " over " << node->Head().Content();
        return ExtractPredicateFromFlatmapOverFlatListIf(*node, ctx);
    }

    if (node->Head().IsCallable({"ToStream", "ToFlow"}) && IsJustOrSingleAsList(node->Head().Head()) && !lambdaArg.IsUsedInDependsOn()) {
        YQL_CLOG(DEBUG, Core) << "Swap " << node->Content() << " with " << node->Head().Content() << " over " << node->Head().Head().Content();
        return ctx.SwapWithHead(*node);
    }

    if (IsJustOrSingleAsList(node->Head()) && !lambdaArg.IsUsedInDependsOn()) {
        YQL_CLOG(DEBUG, Core) << "Fuse " << node->Content() << " over " << node->Head().Content();
        return FuseJustOrSingleAsListWithFlatmap(node, ctx);
    }

    if (node->Head().IsCallable("ToList")) {
        YQL_CLOG(DEBUG, Core) << "Fuse " << node->Content() << " over " << node->Head().Content();
        return FuseToListWithFlatmap(node, ctx);
    }

    if (node->Head().IsCallable("FromFlow")) {
        if (ETypeAnnotationKind::Stream == node->GetTypeAnn()->GetKind()) {
            YQL_CLOG(DEBUG, Core) << "Swap " << node->Content() << " with " << node->Head().Content();
            return ctx.SwapWithHead(*node);
        } else {
            YQL_CLOG(DEBUG, Core) << "Drop " << node->Head().Content() << " under " << node->Content();
            return ctx.ChangeChild(*node, 0U, node->Head().HeadPtr());
        }
    }

    if (lambdaBody.IsCallable("AsList") && lambdaBody.ChildrenSize() == 1 &&
        node->Head().GetTypeAnn()->GetKind() != ETypeAnnotationKind::Optional)
    {
        YQL_CLOG(DEBUG, Core) << node->Content() << " with single arg AsList";
        auto newLambda = ctx.ChangeChild(self.Lambda().Ref(), 1U, ctx.RenameNode(lambdaBody, "Just"));
        return ctx.ChangeChild(*node, 1U, ctx.DeepCopyLambda(*newLambda));
    }

    if (IsJustOrSingleAsList(lambdaBody)) {
        const bool isIdentical = &lambdaBody.Head() == &lambdaArg;
        const auto type = lambdaArg.GetTypeAnn();
        const bool sameType = IsSameAnnotation(*lambdaBody.Head().GetTypeAnn(), *type);
        const bool toList = self.Ref().GetTypeAnn()->GetKind() == ETypeAnnotationKind::List
                            && self.Input().Ref().GetTypeAnn()->GetKind() == ETypeAnnotationKind::Optional;
        if (isIdentical || (sameType && type->IsSingleton())) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " with " << lambdaBody.Content();
            return ctx.WrapByCallableIf(toList, "ToList", self.Input().Ptr());
        }

        auto maybeStruct = TMaybeNode<TCoAsStruct>(lambdaBody.ChildPtr(0));
        if (maybeStruct && type->GetKind() == ETypeAnnotationKind::Struct ) {
            bool replaceByExtractMembers = true;
            TMap<TStringBuf, TPositionHandle> membersToExtract;

            for (auto child : maybeStruct.Cast()) {
                auto tuple = child.Cast<TCoNameValueTuple>();
                auto value = tuple.Value();

                if (auto maybeMember = value.Maybe<TCoMember>()) {
                    auto member = maybeMember.Cast();
                    if (member.Struct().Raw() == &lambdaArg) {
                        TStringBuf inputName = member.Name().Value();
                        TStringBuf outputName = tuple.Name().Value();
                        if (inputName == outputName) {
                            membersToExtract[inputName] = member.Name().Pos();
                            continue;
                        }
                    }
                }
                replaceByExtractMembers = false;
                break;
            }

            if (replaceByExtractMembers) {
                TExprNodeList members;
                members.reserve(membersToExtract.size());
                for (const auto& m : membersToExtract) {
                    members.push_back(ctx.NewAtom(m.second, m.first));
                }


                auto extractMembers = ctx.Builder(node->Pos())
                    .Callable("ExtractMembers")
                        .Add(0, self.Input().Ptr())
                        .Add(1, ctx.NewList(node->Pos(), std::move(members)))
                    .Seal()
                    .Build();

                YQL_CLOG(DEBUG, Core) << node->Content() << " to ExtractMembers";
                return ctx.WrapByCallableIf(toList, "ToList", std::move(extractMembers));
            }
        }
    }

    if (CanRewriteToEmptyContainer(*node)) {
        const auto& inputToCheck = SkipCallables(node->Head(), SkippableCallables);
        if (IsEmptyContainer(inputToCheck) || IsEmpty(inputToCheck, *optCtx.Types)) {
            YQL_CLOG(DEBUG, Core) << "Empty " << node->Content() << " over " << inputToCheck.Content();
            auto res = ctx.NewCallable(inputToCheck.Pos(), GetEmptyCollectionName(node->GetTypeAnn()), {ExpandType(node->Pos(), *node->GetTypeAnn(), ctx)});
            return KeepConstraints(res, *node, ctx);
        }

        const auto& lambdaRootToCheck = SkipCallables(node->Tail().Tail(), SkippableCallables);
        if (IsEmptyContainer(lambdaRootToCheck) || IsEmpty(lambdaRootToCheck, *optCtx.Types)) {
            YQL_CLOG(DEBUG, Core) << "Empty " << node->Content() << " with " << lambdaRootToCheck.Content();
            auto res = ctx.NewCallable(lambdaRootToCheck.Pos(), GetEmptyCollectionName(node->GetTypeAnn()), {ExpandType(node->Pos(), *node->GetTypeAnn(), ctx)});
            return KeepConstraints(res, *node, ctx);
        }
    }

    // rewrite in 'canonical' way (prefer OptionalIf to ListIf)
    if (self.Input().Ref().GetTypeAnn()->GetKind() != ETypeAnnotationKind::Optional && self.Lambda().Body().Maybe<TCoListIf>())
    {
        YQL_CLOG(DEBUG, Core) << "Convert " << node->Content() << " lambda ListIf to OptionalIf";
        auto listIf = self.Lambda().Body().Cast<TCoListIf>();

        auto newLambda = Build<TCoLambda>(ctx, node->Pos())
                .Args({"item"})
                .Body<TCoOptionalIf>()
                    .Predicate<TExprApplier>()
                        .Apply(listIf.Predicate())
                            .With(self.Lambda().Args().Arg(0), "item")
                        .Build()
                    .Value<TExprApplier>()
                        .Apply(listIf.Value())
                            .With(self.Lambda().Args().Arg(0), "item")
                        .Build()
                    .Build()
                .Done().Ptr();

        return ctx.ChangeChild(*node, 1U, std::move(newLambda));
    }

    if (auto expr = TryConvertSqlInPredicatesToJoins(self, ShouldConvertSqlInToJoin, ctx)) {
        return expr;
    }

    if (auto just = self.Lambda().Body().Maybe<TCoJust>()) {
        if (auto tuple = just.Cast().Input().Maybe<TExprList>()) {
            if (tuple.Cast().Size() > 0) {
                TExprNode::TPtr inner;
                for (ui32 i = 0; i < tuple.Cast().Size(); ++i) {
                    auto x = tuple.Cast().Item(i).Raw();
                    if (!x->IsCallable("Nth") || x->Child(1)->Content() != ToString(i)) {
                        inner = nullptr;
                        break;
                    }

                    auto current = x->ChildPtr(0);
                    if (current != self.Lambda().Args().Arg(0).Raw()) {
                        inner = nullptr;
                        break;
                    }

                    if (!inner) {
                        inner = current;
                    } else if (inner != current) {
                        inner = nullptr;
                        break;
                    }

                    if (inner->GetTypeAnn()->GetKind() != ETypeAnnotationKind::Tuple) {
                        inner = nullptr;
                        break;
                    }

                    if (inner->GetTypeAnn()->Cast<TTupleExprType>()->GetSize() != tuple.Cast().Size()) {
                        inner = nullptr;
                        break;
                    }
                }

                if (inner) {
                    YQL_CLOG(DEBUG, Core) << "Skip tuple rebuild in  " << node->Content();
                    return self.Input().Ptr();
                }
            }
        }
    }

    return node;
}

TExprNode::TPtr HasNullOverTuple(const TExprNode::TPtr& node, TExprContext& ctx) {
    auto value = node->HeadPtr();

    TExprNode::TListType predicates;
    for (auto i : xrange(value->GetTypeAnn()->Cast<TTupleExprType>()->GetSize())) {
        predicates.push_back(ctx.Builder(node->Pos())
            .Callable("HasNull")
                .Callable(0, "Nth")
                    .Add(0, value)
                    .Atom(1, ToString(i), TNodeFlags::Default)
                .Seal()
            .Seal()
            .Build());
    }

    if (predicates.empty()) {
        return MakeBool<false>(node->Pos(), ctx);
    }

    return ctx.NewCallable(node->Pos(), "Or", std::move(predicates));
}

TExprNode::TPtr HasNullOverStruct(const TExprNode::TPtr& node, TExprContext& ctx) {
    auto value = node->HeadPtr();

    TExprNode::TListType predicates;
    for (auto& item : value->GetTypeAnn()->Cast<TStructExprType>()->GetItems()) {
        predicates.push_back(ctx.Builder(node->Pos())
            .Callable("HasNull")
                .Callable(0, "Member")
                    .Add(0, value)
                    .Atom(1, item->GetName())
                .Seal()
            .Seal()
            .Build());
    }

    if (predicates.empty()) {
        return MakeBool<false>(node->Pos(), ctx);
    }

    return ctx.NewCallable(node->Pos(), "Or", std::move(predicates));
}

TExprNode::TPtr HasNullOverVariant(const TExprNode::TPtr& node, TExprContext& ctx) {
    auto value = node->HeadPtr();

    auto underlyingType = value->GetTypeAnn()->Cast<TVariantExprType>()->GetUnderlyingType();

    const size_t size = underlyingType->GetKind() == ETypeAnnotationKind::Struct ?
                        underlyingType->Cast<TStructExprType>()->GetSize() :
                        underlyingType->Cast<TTupleExprType>()->GetSize();

    return ctx.Builder(node->Pos())
        .Callable("Visit")
        .Add(0, value)
        .Do([&](TExprNodeBuilder& parent) -> TExprNodeBuilder& {
            for (auto i : xrange(size)) {
                TString index;
                if (underlyingType->GetKind() == ETypeAnnotationKind::Struct) {
                    index = underlyingType->Cast<TStructExprType>()->GetItems()[i]->GetName();
                } else {
                    index = ToString(i);
                }

                parent
                    .Atom(2 * i + 1, index)
                    .Lambda(2 * i + 2)
                        .Param("item")
                        .Callable("HasNull")
                            .Arg(0, "item")
                        .Seal()
                    .Seal();
            }
            return parent;
        })
        .Seal()
        .Build();

}

TExprNode::TPtr OptimizeToFlow(const TExprNode::TPtr& node, TExprContext& ctx) {
    if (node->Head().IsCallable("Nothing")) {
        YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
        return ctx.NewCallable(node->Pos(), "EmptyIterator", {ExpandType(node->Pos(), *node->GetTypeAnn(), ctx)});
    }

    if (node->Head().IsCallable({"ForwardList", "LazyList", "ToStream"})) {
        YQL_CLOG(DEBUG, Core) << "Drop " << node->Head().Content() << " under " << node->Content();
        return ctx.ChangeChildren(*node, node->Head().ChildrenList());
    }

    if (1U == node->Head().ChildrenSize() && node->Head().IsCallable("Iterator") && ETypeAnnotationKind::List == node->Head().Head().GetTypeAnn()->GetKind()) {
        YQL_CLOG(DEBUG, Core) << "Drop " << node->Head().Content() << " under " << node->Content();
        return ctx.ChangeChildren(*node, node->Head().ChildrenList());
    }

    return node;
}

TExprNode::TPtr OptimizeCollect(const TExprNode::TPtr& node, TExprContext& ctx) {
    if (node->Head().IsCallable({"ForwardList", "LazyList"})) {
        YQL_CLOG(DEBUG, Core) << "Drop " << node->Head().Content() << " under " << node->Content();
        return ctx.ChangeChildren(*node, node->Head().ChildrenList());
    }

    if (1U == node->Head().ChildrenSize() && node->Head().IsCallable("Iterator") && ETypeAnnotationKind::List == node->Head().Head().GetTypeAnn()->GetKind()) {
        YQL_CLOG(DEBUG, Core) << "Drop " << node->Head().Content() << " under " << node->Content();
        return ctx.ChangeChildren(*node, node->Head().ChildrenList());
    }

    const auto& nodeToCheck = SkipCallables(node->Head(), SkippableCallables);
    if (nodeToCheck.IsCallable({node->Content(), "List", "ListIf", "AsList"})) {
        YQL_CLOG(DEBUG, Core) << "Drop " << node->Content() <<  " over " << nodeToCheck.Content();
        return node->HeadPtr();
    }

    return node;
}

TExprNode::TPtr DropDuplicate(const TExprNode::TPtr& node, TExprContext&) {
    if (node->Head().IsCallable(node->Content())) {
        YQL_CLOG(DEBUG, Core) << "Drop duplicate of " << node->Content();
        return node->Head().HeadPtr();
    }

    return node;
}

template <bool Strong>
TExprNode::TPtr OptimizeCast(const TExprNode::TPtr& node, TExprContext& ctx) {
    if (node->Head().IsCallable("Nothing") && GetOptionalLevel(node->GetTypeAnn()) <= GetOptionalLevel(node->Head().GetTypeAnn())) {
        YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
        if (ETypeAnnotationKind::Null == node->GetTypeAnn()->Cast<TOptionalExprType>()->GetItemType()->GetKind()) {
            return ctx.NewCallable(node->Head().Pos(), "Just", {ctx.NewCallable(node->Head().Pos(), "Null", {})});
        }

        return ctx.ChangeChild(node->Head(), 0U, ExpandType(node->Pos(), *node->GetTypeAnn(), ctx));
    }

    if (node->Head().IsCallable("Just")) {
        YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
        auto type = ExpandType(node->Pos(), *node->GetTypeAnn(), ctx);
        return ctx.ChangeChildren(*node, {node->Head().HeadPtr(), std::move(type)});
    }

    if (GetOptionalLevel(node->GetTypeAnn()) > GetOptionalLevel(node->Head().GetTypeAnn())) {
        const auto itemType = node->GetTypeAnn()->Cast<TOptionalExprType>()->GetItemType();
        if (!(NKikimr::NUdf::ECastOptions::MayFail & CastResult<Strong>(node->Head().GetTypeAnn(), itemType))) {
            YQL_CLOG(DEBUG, Core) << "Pull out Just from " << node->Content();
            auto type = ExpandType(node->Pos(), *itemType, ctx);
            return ctx.NewCallable(node->Pos(), "Just", {ctx.ChangeChild(*node, 1U, std::move(type))});
        }
    }

    return node;
}

template <bool TakeOrSkip, bool Inclusive = false>
TExprNode::TPtr OptimizeWhile(const TExprNode::TPtr& node, TExprContext& ctx) {
    const auto& emptyCollectionName = GetEmptyCollectionName(node->GetTypeAnn());
    const auto& nodeToCheck = SkipCallables(node->Head(), SkippableCallables);
    if (1U == nodeToCheck.ChildrenSize() && nodeToCheck.IsCallable(emptyCollectionName)) {
        YQL_CLOG(DEBUG, Core) << node->Content() << " over empty " << nodeToCheck.Content();
        return node->HeadPtr();
    }

    const auto& lambdaBody = node->Tail().Tail();
    if (lambdaBody.IsCallable("Bool")) {
        YQL_CLOG(DEBUG, Core) << node->Content() << " with lambda " << lambdaBody.Content() << " '" << lambdaBody.Head().Content();
        const bool isAll = FromString<bool>(lambdaBody.Head().Content());
        return TakeOrSkip == isAll
            ? node->HeadPtr()
            : Inclusive
                ? ctx.Builder(lambdaBody.Pos())
                    .Callable("Take")
                        .Add(0, node->HeadPtr())
                        .Callable(1, "Uint64")
                            .Atom(0, "1", TNodeFlags::Default)
                        .Seal()
                    .Seal().Build()
                : KeepConstraints(
                    ctx.NewCallable(lambdaBody.Pos(), emptyCollectionName, {ExpandType(node->Pos(), *node->GetTypeAnn(), ctx)}),
                    *node, ctx);
    }
    return node;
}

template <bool MinOrMax>
TExprNode::TPtr OptimizeMinMax(const TExprNode::TPtr& node, TExprContext& ctx) {
    bool constIntsOnly = true;
    for (ui32 i = 0; i < node->ChildrenSize(); ++i) {
        if (node->Child(i)->IsCallable("Nothing")) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
            return node->ChildPtr(i);
        }
        constIntsOnly = constIntsOnly && TMaybeNode<TCoIntegralCtor>(node->Child(i));
    }

    if (constIntsOnly && node->ChildrenSize() > 0) {
        auto result = (MinOrMax ? &ConstFoldNodeIntAggregate<TMinAggregate> : &ConstFoldNodeIntAggregate<TMaxAggregate>)(node, ctx);
        if (result != node) {
            YQL_CLOG(DEBUG, Core) << "Constant fold " << node->Content() << " over integrals.";
            return result;
        }
    }

    return node;
}

TExprNode::TPtr OptimizeCompare(const TExprNode::TPtr& node, TExprContext& ctx) {
    if (node->Head().IsCallable("Nothing") || node->Tail().IsCallable("Nothing")) {
        YQL_CLOG(DEBUG, Core) << "Compare '" << node->Content() << "' over Nothing";
        return MakeBoolNothing(node->Pos(), ctx);
    }

    return node;
}

TExprNode::TPtr DropReorder(const TExprNode::TPtr& node, TExprContext& ctx) {
    if (IsListReorder(node->Head())) {
        YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
        return ctx.ChangeChild(*node, 0U, node->Head().HeadPtr());
    }

    return node;
}

template <bool IsTop, bool IsSort>
TExprNode::TPtr OptimizeReorder(const TExprNode::TPtr& node, TExprContext& ctx) {
    const ui32 ascIndex = node->ChildrenSize() - 2U;
    if ((IsSort || IsTop) && 1U == node->Head().ChildrenSize() && node->Head().IsCallable(GetEmptyCollectionName(node->Head().GetTypeAnn()))) {
        YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
        return IsSort ?
            ctx.Builder(node->Pos())
                .Callable("AssumeSorted")
                    .Add(0, node->HeadPtr())
                    .Add(1, node->ChildPtr(ascIndex))
                    .Add(2, node->TailPtr())
                .Seal().Build():
            node->HeadPtr();
    }

    if (IsSort && IsListReorder(node->Head())) {
        YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
        return ctx.ChangeChild(*node, 0U, node->Head().HeadPtr());
    }

    if (const auto& lambda = node->Tail(); lambda.Tail().GetDependencyScope()->second != &lambda) {
        YQL_CLOG(DEBUG, Core) << node->Content() << " by constant";
        return IsTop ?
            ctx.Builder(node->Pos())
                .Callable("Take")
                    .Add(0, node->HeadPtr())
                    .Add(1, node->ChildPtr(1))
                .Seal().Build():
            node->HeadPtr();
    }

    if (node->Child(ascIndex)->GetTypeAnn()->GetKind() == ETypeAnnotationKind::Tuple &&
        node->Child(ascIndex)->GetTypeAnn()->Cast<TTupleExprType>()->GetSize() == 1U) {
        YQL_CLOG(DEBUG, Core) << node->Content() << " unpack single item ascending";
        auto unpack = node->Child(ascIndex)->IsList() ?
            node->Child(ascIndex)->HeadPtr():
            ctx.Builder(node->Pos())
            .Callable("Nth")
                .Add(0, node->ChildPtr(ascIndex))
                .Atom(1, "0", TNodeFlags::Default)
            .Seal().Build();
        return ctx.ChangeChild(*node, ascIndex, {std::move(unpack)});
    }

    if (node->Tail().Tail().GetTypeAnn()->GetKind() == ETypeAnnotationKind::Tuple) {
        const auto keyType = node->Tail().Tail().GetTypeAnn()->Cast<TTupleExprType>();
        if (1U == keyType->GetSize()) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " unpack single item tuple";
            auto unpack = node->Tail().Tail().IsList() ?
                ctx.Builder(node->Tail().Pos())
                    .Lambda()
                        .Param("input")
                        .ApplyPartial(node->Tail().HeadPtr(), node->Tail().Tail().HeadPtr())
                            .With(0, "input")
                        .Seal()
                    .Seal().Build():
                ctx.Builder(node->Tail().Pos())
                    .Lambda()
                        .Param("input")
                        .Callable("Nth")
                            .Apply(0, node->TailPtr()).With(0, "input").Seal()
                            .Atom(1, "0", TNodeFlags::Default)
                        .Seal()
                    .Seal().Build();
            return ctx.ChangeChild(*node, node->ChildrenSize() - 1U, {std::move(unpack)});
        }
    }

    if (IsTop) {
        if (node->Child(1)->IsCallable("Uint64")) {
            const ui64 count = FromString<ui64>(node->Child(1)->Head().Content());
            if (0 == count) {
                YQL_CLOG(DEBUG, Core) << node->Content() << " with zero count";
                auto res = ctx.NewCallable(node->Pos(), GetEmptyCollectionName(node->Head().GetTypeAnn()), {ExpandType(node->Pos(), *node->Head().GetTypeAnn(), ctx)});
                if (IsSort) {
                    res = ctx.Builder(node->Pos())
                        .Callable("AssumeSorted")
                            .Add(0, std::move(res))
                            .Add(1, node->ChildPtr(ascIndex))
                            .Add(2, node->TailPtr())
                        .Seal().Build();
                }
                return res;
            }

            if (node->Head().IsCallable({"List", "AsList"})) {
                size_t listSize = node->Head().ChildrenSize();
                if (node->Head().IsCallable("List")) {
                    --listSize;
                }

                if (listSize <= count) {
                    YQL_CLOG(DEBUG, Core) << node->Content() << " over " << listSize << " literals";
                    return IsSort ?
                        ctx.Builder(node->Pos())
                            .Callable(listSize > 1U ? "Sort" : "AssumeSorted")
                                .Add(0, node->HeadPtr())
                                .Add(1, node->ChildPtr(ascIndex))
                                .Add(2, node->TailPtr())
                            .Seal().Build():
                        node->HeadPtr();
                }
            }
        }

        if (auto inputConstr = node->Head().GetConstraint<TSortedConstraintNode>()) {
            if (auto topConstr = node->GetConstraint<TSortedConstraintNode>()) {
                if (topConstr->IsPrefixOf(*inputConstr)) {
                    YQL_CLOG(DEBUG, Core) << node->Content() << " over sorted input";

                    auto res = ctx.Builder(node->Pos())
                        .Callable("Take")
                            .Add(0, node->HeadPtr())
                            .Add(1, node->ChildPtr(1))
                        .Seal()
                        .Build();

                    if (topConstr->Equals(*inputConstr)) {
                        return res;
                    }

                    return KeepSortedConstraint(res, topConstr, ctx);
                }
            }
        }
    }

    if (IsSort) {
        const auto& nodeToCheck = SkipCallables(node->Head(), SkippableCallables);
        if (nodeToCheck.IsCallable({"List", "AsList"})) {
            ui32 count = nodeToCheck.ChildrenSize();
            if (nodeToCheck.IsCallable("List")) {
                --count;
            }

            if (count <= 1) {
                YQL_CLOG(DEBUG, Core) << node->Content() << " over 0/1 literals";
                return ctx.RenameNode(*node, "AssumeSorted");
            }
        }

        if (const auto inputConstr = node->Head().GetConstraint<TSortedConstraintNode>()) {
            if (const auto sortConstr = node->GetConstraint<TSortedConstraintNode>()) {
                if (sortConstr->IsPrefixOf(*inputConstr)) {
                    YQL_CLOG(DEBUG, Core) << node->Content() << " over sorted input";
                    return KeepSortedConstraint(node->HeadPtr(), sortConstr, ctx);
                }
            }
        }
    } else if (!IsTop) {
        if (node->Head().IsCallable(node->Content())) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
            return ctx.ChangeChild(*node, 0U, node->Head().HeadPtr());
        }
    }

    return node;
}

void FixSortness(const TExprNode& origNode, TExprNode::TPtr& node, TExprContext& ctx) {
    if (auto sorted = origNode.GetConstraint<TSortedConstraintNode>()) {
        const auto& content = sorted->GetContent();
        node = ctx.Builder(origNode.Pos())
            .Callable("Sort")
                .Add(0, std::move(node))
                .List(1)
                    .Do([&](TExprNodeBuilder& parent) -> TExprNodeBuilder& {
                        size_t index = 0;
                        for (auto c : content) {
                            parent.Callable(index++, "Bool")
                                    .Atom(0, ToString(c.second), TNodeFlags::Default)
                                    .Seal();
                        }
                        return parent;
                    })
                .Seal()
                .Lambda(2)
                    .Param("item")
                    .List()
                        .Do([&](TExprNodeBuilder& parent) -> TExprNodeBuilder& {
                            size_t index = 0;
                            for (auto c : content) {
                                parent.Callable(index++, "Member")
                                        .Arg(0, "item")
                                        .Atom(1, c.first.front())
                                        .Seal();
                            }
                            return parent;
                        })
                    .Seal()
                .Seal()
            .Seal()
            .Build();
    }
}

TExprNode::TPtr ConvertSqlInPredicatesPrefixToJoins(const TExprNode::TPtr& flatMap, const TPredicateChain& chain,
    const TExprNode::TPtr& sqlInTail, TExprContext& ctx)
{
    YQL_ENSURE(!chain.empty());
    YQL_ENSURE(chain.front().ConvertibleToJoin);

    TExprNodeList extraLefts;
    TExprNodeList extraRights;
    for (auto& node : chain) {
        if (node.ExtraLeftPred) {
            extraLefts.push_back(node.ExtraLeftPred);
        }
        if (node.ExtraRightPred) {
            extraRights.push_back(node.ExtraRightPred);
        }
    }

    if (!extraLefts.empty() || !extraRights.empty())
    {
        TExprNodeList predicates;
        predicates.reserve(extraLefts.size() + extraRights.size() + chain.size() + 1);

        predicates.insert(predicates.end(), extraLefts.begin(), extraLefts.end());
        predicates.insert(predicates.end(), extraRights.begin(), extraRights.end());

        for (auto& node: chain) {
            YQL_ENSURE(node.Pred);
            predicates.push_back(node.Pred);
        }

        if (sqlInTail) {
            predicates.push_back(sqlInTail);
        }

        YQL_CLOG(DEBUG, Core) << "FlatMapOverJoinableSqlInChain of size " << chain.size() << " with "
                              << extraLefts.size()  << " extra left predicates and "
                              << extraRights.size() << " extra right predicates due to NOT IN";

        auto combinedPred = ctx.NewCallable(predicates.front()->Pos(), "And", std::move(predicates));
        return RebuildFlatmapOverPartOfPredicate(flatMap, flatMap->HeadPtr(), combinedPred, true, ctx);
    }

    YQL_CLOG(DEBUG, Core) << "FlatMapOverJoinableSqlInChain of size " << chain.size();

    auto eq = BuildEquiJoinForSqlInChain(flatMap, chain, ctx);
    FixSortness(*flatMap, eq, ctx);

    auto tail = sqlInTail ? sqlInTail : MakeBool<true>(flatMap->Pos(), ctx);
    return RebuildFlatmapOverPartOfPredicate(flatMap, eq, tail, true, ctx);
}

TExprNode::TPtr ConvertSqlInPredicatesToJoins(const TCoFlatMapToEquiJoinBase& flatMap, TExprContext& ctx) {
    TCoLambda lambda = flatMap.Lambda();
    YQL_ENSURE(lambda.Body().Maybe<TCoOptionalIf>());

    TPredicateChain chain;
    auto lambdaArg = lambda.Ptr()->Head().HeadPtr();
    auto sqlInTail = SplitPredicateChain(lambda.Ptr()->Child(1)->HeadPtr(), lambdaArg, ShouldConvertSqlInToJoin, chain, ctx);
    return ConvertSqlInPredicatesPrefixToJoins(flatMap.Ptr(), chain, sqlInTail, ctx);
}

TExprNodeList DeduplicateAndSplitTupleCollectionByTypes(const TExprNode &collection, TExprContext &ctx) {
    const auto& tupleItemsTypes = collection.GetTypeAnn()->Cast<TTupleExprType>()->GetItems();

    TVector<TExprNodeList> collections;
    THashMap<const TTypeAnnotationNode*, size_t> indexByType;
    THashSet<const TExprNode*> uniqNodes;

    for (size_t i = 0; i < tupleItemsTypes.size(); ++i) {
        auto item = collection.ChildPtr(i);
        if (uniqNodes.contains(item.Get())) {
            continue;
        }
        uniqNodes.insert(item.Get());

        auto itemType = tupleItemsTypes[i];

        size_t idx;
        auto it = indexByType.find(itemType);
        if (it == indexByType.end()) {
            idx = collections.size();
            indexByType[itemType] = idx;
            collections.emplace_back();
        } else {
            idx = it->second;
        }

        collections[idx].push_back(item);
    }

    TExprNodeList result;
    result.reserve(collections.size());

    for (auto& c : collections) {
        result.push_back(ctx.NewList(collection.Pos(), std::move(c)));
    }

    return result;
}

TExprNode::TPtr MergeCalcOverWindowFrames(const TExprNode::TPtr& frames, TExprContext& ctx) {
    YQL_ENSURE(frames->IsList());


    TNodeMap<size_t> uniqueFrameIndexes;
    struct TWinOnRowsContent {
        TExprNodeList Args;
        TPositionHandle Pos;
    };

    TVector<TWinOnRowsContent> winOnRows;

    for (auto& winOn: frames->Children()) {
        YQL_ENSURE(winOn->IsCallable("WinOnRows"));

        if (winOn->ChildrenSize() == 1) {
            // skip empty frames
            continue;
        }
        auto args = winOn->ChildrenList();
        auto frameSpec = winOn->Child(0);
        auto frameIt = uniqueFrameIndexes.find(frameSpec);
        if (frameIt == uniqueFrameIndexes.end()) {
            YQL_ENSURE(uniqueFrameIndexes.size() == winOnRows.size());
            uniqueFrameIndexes[frameSpec] = winOnRows.size();

            TWinOnRowsContent content{std::move(args), winOn->Pos()};
            winOnRows.emplace_back(std::move(content));
        } else {
            auto& combined = winOnRows[frameIt->second];
            combined.Args.insert(combined.Args.end(), args.begin() + 1, args.end());
        }
    }

    if (uniqueFrameIndexes.size() != frames->ChildrenSize()) {
        TExprNodeList winOnRowsNodes;
        for (auto &item : winOnRows) {
            winOnRowsNodes.emplace_back(ctx.NewCallable(item.Pos, "WinOnRows", std::move(item.Args)));
        }
        return ctx.NewList(frames->Pos(), std::move(winOnRowsNodes));
    }

    return frames;
}

TExprNodeList DedupCalcOverWindowsOnSamePartitioning(const TExprNodeList& calcs, TExprContext& ctx) {
    struct TDedupKey {
        const TExprNode* Keys = nullptr;
        const TExprNode* SortSpec = nullptr;
        const TExprNode* SessionSpec = nullptr;
        bool operator<(const TDedupKey& other) const {
            return std::tie(Keys, SortSpec, SessionSpec) < std::tie(other.Keys, other.SortSpec, other.SessionSpec);
        }
    };

    TMap<TDedupKey, size_t> uniqueIndexes;
    TExprNodeList uniqueCalcs;
    for (auto& calcNode : calcs) {
        TCoCalcOverWindowTuple calc(calcNode);
        if (calc.Frames().Size() == 0 && calc.SessionColumns().Size() == 0) {
            continue;
        }
        TDedupKey key{calc.Keys().Raw(), calc.SortSpec().Raw(), calc.SessionSpec().Raw()};

        auto it = uniqueIndexes.find(key);
        if (it == uniqueIndexes.end()) {
            YQL_ENSURE(uniqueIndexes.size() == uniqueCalcs.size());
            const size_t idx = uniqueCalcs.size();
            uniqueIndexes[key] = idx;
            uniqueCalcs.emplace_back(calc.Ptr());
        } else {
            const size_t idx = it->second;
            TCoCalcOverWindowTuple existing(uniqueCalcs[idx]);

            auto existingFrames = existing.Frames().Ref().ChildrenList();
            auto existingSessionColumns = existing.SessionColumns().Ref().ChildrenList();

            auto frames = calc.Frames().Ref().ChildrenList();
            auto sessionColumns = calc.SessionColumns().Ref().ChildrenList();

            frames.insert(frames.end(), existingFrames.begin(), existingFrames.end());
            sessionColumns.insert(sessionColumns.end(), existingSessionColumns.begin(), existingSessionColumns.end());

            uniqueCalcs[idx] = Build<TCoCalcOverWindowTuple>(ctx, calc.Pos())
                .Keys(calc.Keys())
                .SortSpec(calc.SortSpec())
                .Frames(ctx.NewList(calc.Frames().Pos(), std::move(frames)))
                .SessionSpec(calc.SessionSpec())
                .SessionColumns(ctx.NewList(calc.SessionColumns().Pos(), std::move(sessionColumns)))
                .Done().Ptr();
        }
    }
    return uniqueCalcs;
}

TExprNode::TPtr BuildCalcOverWindowGroup(TCoCalcOverWindowGroup node, TExprNodeList&& calcs, TExprContext& ctx) {
    if (calcs.size() == 0) {
        return node.Input().Ptr();
    }

    if (calcs.size() == 1) {
        TCoCalcOverWindowTuple calc(calcs[0]);
        if (calc.SessionSpec().Maybe<TCoVoid>()) {
            YQL_ENSURE(calc.SessionColumns().Size() == 0);
            return Build<TCoCalcOverWindow>(ctx, node.Pos())
                .Input(node.Input())
                .Keys(calc.Keys())
                .SortSpec(calc.SortSpec())
                .Frames(calc.Frames())
                .Done().Ptr();
        } else {
            return Build<TCoCalcOverSessionWindow>(ctx, node.Pos())
                .Input(node.Input())
                .Keys(calc.Keys())
                .SortSpec(calc.SortSpec())
                .Frames(calc.Frames())
                .SessionSpec(calc.SessionSpec())
                .SessionColumns(calc.SessionColumns())
                .Done().Ptr();
        }
    }

    return Build<TCoCalcOverWindowGroup>(ctx, node.Pos())
        .Input(node.Input())
        .Calcs(ctx.NewList(node.Pos(), std::move(calcs)))
        .Done().Ptr();
}

bool HasPayload(const TCoAggregate& node) {
    return node.Handlers().Size() > 0 || HasSetting(node.Settings().Ref(), "hopping");
}

TExprNode::TPtr PullAssumeColumnOrderOverEquiJoin(const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) {
    TVector<ui32> withAssume;
    for (ui32 i = 0; i < node->ChildrenSize() - 2; i++) {
        if (node->Child(i)->Child(0)->IsCallable("AssumeColumnOrder")) {
            withAssume.push_back(i);
        }
    }

    if (withAssume) {
        YQL_CLOG(DEBUG, Core) << "Pull AssumeColumnOrder over " << node->Content();
        auto inputs = node->ChildrenList();
        for (ui32 idx : withAssume) {
            inputs[idx] = ctx.NewList(inputs[idx]->Pos(), { inputs[idx]->Child(0)->ChildPtr(0), inputs[idx]->ChildPtr(1)});
        }
        auto result = ctx.ChangeChildren(*node, std::move(inputs));
        return KeepColumnOrder(result, *node, ctx, *optCtx.Types);
    }
    return node;
}

} // namespace

TExprNode::TPtr TryConvertSqlInPredicatesToJoins(const TCoFlatMapBase& flatMap,
    TShouldConvertSqlInToJoinPredicate shouldConvertSqlInToJoin, TExprContext& ctx, bool prefixOnly)
{
    // FlatMap input should be List<Struct<...>> to be accepted as EquiJoin input
    auto inputType = flatMap.Input().Ref().GetTypeAnn();
    if (inputType->GetKind() != ETypeAnnotationKind::List ||
        inputType->Cast<TListExprType>()->GetItemType()->GetKind() != ETypeAnnotationKind::Struct)
    {
        return {};
    }

    TCoLambda lambda = flatMap.Lambda();
    if (!lambda.Body().Maybe<TCoConditionalValueBase>()) {
        return {};
    }

    TCoConditionalValueBase conditional(lambda.Body().Ptr());
    TPredicateChain chain;
    auto lambdaArg = lambda.Ptr()->Head().HeadPtr();
    auto sqlInTail = SplitPredicateChain(conditional.Predicate().Ptr(), lambdaArg, shouldConvertSqlInToJoin, chain, ctx);

    if (!chain.empty()) {
        if (chain.front().ConvertibleToJoin) {
            return ConvertSqlInPredicatesPrefixToJoins(flatMap.Ptr(), chain, sqlInTail, ctx);
        }

        if (sqlInTail && !prefixOnly) {
            YQL_CLOG(DEBUG, Core) << "FlatMapOverNonJoinableSqlInChain of size " << chain.size();
            TExprNode::TListType predicates;
            predicates.reserve(chain.size());
            for (auto& it : chain) {
                predicates.emplace_back(std::move(it.Pred));
            }
            auto prefixPred = ctx.NewCallable(flatMap.Pos(), "And", std::move(predicates));

            auto innerFlatMap = RebuildFlatmapOverPartOfPredicate(flatMap.Ptr(), flatMap.Input().Ptr(), prefixPred, false, ctx);
            auto outerFlatMap = RebuildFlatmapOverPartOfPredicate(flatMap.Ptr(), innerFlatMap, sqlInTail, true, ctx);
            return ctx.RenameNode(*outerFlatMap,
                outerFlatMap->Content() == "OrderedFlatMap" ? "OrderedFlatMapToEquiJoin" : "FlatMapToEquiJoin");
        }
    }

    return {};
}

TExprNode::TPtr FoldParseAfterSerialize(const TExprNode::TPtr& node, const TStringBuf parseUdfName, const THashSet<TStringBuf>& serializeUdfNames) { 
    auto apply = TExprBase(node).Cast<TCoApply>();

    auto outerUdf = apply.Arg(0).Maybe<TCoUdf>();
    if (!outerUdf || outerUdf.Cast().MethodName() != parseUdfName) { 
        return node;
    }

    auto directCase = [&](const TCoApply& apply) { 
        auto node = apply.Ptr();
        auto maybeUdfApply = apply.Arg(1).Maybe<TCoApply>();
        if (!maybeUdfApply) {
            return node;
        }

        auto maybePairUdf = maybeUdfApply.Cast().Arg(0).Maybe<TCoUdf>();
        if (!maybePairUdf || !serializeUdfNames.contains(maybePairUdf.Cast().MethodName())) { 
            return node;
        }

        YQL_CLOG(DEBUG, Core) << "Drop " << outerUdf.Cast().MethodName().Value() << " over " << maybePairUdf.Cast().MethodName().Value();
        return maybeUdfApply.Cast().Arg(1).Ptr();
    };

    const auto directRes = directCase(apply); 
    if (directRes.Get() != node.Get()) { 
        return directRes; 
    } 
 
    auto flatMapCase = [&](const TCoApply& apply) { 
        auto node = apply.Ptr();
        auto maybeFlatMap = apply.Arg(1).Maybe<TCoFlatMapBase>();
        if (!maybeFlatMap) {
            return node;
        }

        auto flatLambda = maybeFlatMap.Cast().Lambda();

        auto maybeUdfApply = flatLambda.Body().Maybe<TCoJust>().Input().Maybe<TCoApply>();
        if (!maybeUdfApply) {
            return node;
        }

        auto maybePairUdf = maybeUdfApply.Cast().Arg(0).Maybe<TCoUdf>();
        if (!maybePairUdf || !serializeUdfNames.contains(maybePairUdf.Cast().MethodName())) { 
            return node;
        }

        if (flatLambda.Args().Size() != 1 || flatLambda.Args().Arg(0).Raw() != maybeUdfApply.Cast().Arg(1).Raw()) {
            return node;
        }

        return maybeFlatMap.Cast().Input().Ptr();
    };

    return flatMapCase(apply);
}

TExprNode::TPtr FoldYsonParseAfterSerialize(const TExprNode::TPtr& node) { 
    static const THashSet<TStringBuf> serializeUdfNames = {"Yson.Serialize", "Yson.SerializeText", "Yson.SerializePretty"}; 
    return FoldParseAfterSerialize(node, "Yson.Parse", serializeUdfNames); 
} 

TExprNode::TPtr FoldYson2ParseAfterSerialize(const TExprNode::TPtr& node) {
    static const THashSet<TStringBuf> serializeUdfNames = {"Yson2.Serialize", "Yson2.SerializeText", "Yson2.SerializePretty"};
    return FoldParseAfterSerialize(node, "Yson2.Parse", serializeUdfNames);
}

TExprNode::TPtr FoldJsonParseAfterSerialize(const TExprNode::TPtr& node) { 
    static const THashSet<TStringBuf> serializeUdfNames = {"Json2.Serialize"}; 
    return FoldParseAfterSerialize(node, "Json2.Parse", serializeUdfNames); 
} 
 
TExprNode::TPtr FoldSeralizeAfterParse(const TExprNode::TPtr& node, const TStringBuf parseUdfName, const TStringBuf serializeUdfName) { 
    auto apply = TExprBase(node).Cast<TCoApply>();

    auto outerUdf = apply.Arg(0).Maybe<TCoUdf>();
    if (!outerUdf || outerUdf.Cast().MethodName() != serializeUdfName) { 
        return node;
    }

    auto maybeUdfApply = apply.Arg(1).Maybe<TCoApply>();
    if (!maybeUdfApply) {
        return node;
    }

    auto maybePairUdf = maybeUdfApply.Cast().Arg(0).Maybe<TCoUdf>();
    if (!maybePairUdf || maybePairUdf.Cast().MethodName().Value() != parseUdfName) { 
        return node;
    }

    auto innerInput = maybeUdfApply.Cast().Arg(1).Ptr();
    if (RemoveOptionalType(innerInput->GetTypeAnn())->Cast<TDataExprType>()->GetSlot() != EDataSlot::Yson) {
        return node;
    }

    YQL_CLOG(DEBUG, Core) << "Drop " <<  outerUdf.Cast().MethodName().Value() << " over " << maybePairUdf.Cast().MethodName().Value();
    return innerInput;
}

TExprNode::TPtr FoldYsonSeralizeAfterParse(const TExprNode::TPtr& node) { 
    return FoldSeralizeAfterParse(node, "Yson.Parse", "Yson.Serialize"); 
} 
 
TExprNode::TPtr FoldYson2SeralizeAfterParse(const TExprNode::TPtr& node) {
    return FoldSeralizeAfterParse(node, "Yson2.Parse", "Yson2.Serialize");
}

TExprNode::TPtr FoldJsonSeralizeAfterParse(const TExprNode::TPtr& node) { 
    return FoldSeralizeAfterParse(node, "Json2.Parse", "Json2.Serialize"); 
} 
 
TExprNode::TPtr BuildJsonParse(const TExprNode::TPtr& jsonExpr, TExprContext& ctx) { 
    auto jsonPos = jsonExpr->Pos(); 
 
    auto argumentsType = ctx.MakeType<TTupleExprType>(TTypeAnnotationNode::TListType{ 
        ctx.MakeType<TDataExprType>(EDataSlot::Json), 
    }); 
 
    auto udfArgumentsType = ctx.MakeType<TTupleExprType>(TTypeAnnotationNode::TListType{ 
        argumentsType, 
        ctx.MakeType<TStructExprType>(TVector<const TItemExprType*>{}), 
        ctx.MakeType<TTupleExprType>(TTypeAnnotationNode::TListType{}) 
    }); 
 
    auto parse = Build<TCoUdf>(ctx, jsonPos) 
        .MethodName() 
            .Build("Json2.Parse") 
        .RunConfigValue<TCoVoid>() 
            .Build() 
        .UserType(ExpandType(jsonPos, *udfArgumentsType, ctx)) 
        .Done().Ptr(); 
 
    return Build<TCoApply>(ctx, jsonPos) 
        .Callable(parse) 
        .FreeArgs() 
            .Add(jsonExpr) 
            .Build() 
        .Done().Ptr(); 
} 
 
TExprNode::TPtr BuildJsonParse(const TCoJsonQueryBase& jsonExpr, TExprContext& ctx) { 
    return BuildJsonParse(jsonExpr.Json().Ptr(), ctx); 
} 
 
TExprNode::TPtr GetJsonDocumentOrParseJson(const TExprNode::TPtr& jsonExpr, TExprContext& ctx, EDataSlot& argumentDataSlot) { 
    const TTypeAnnotationNode* type = jsonExpr->GetTypeAnn(); 
    if (type->GetKind() == ETypeAnnotationKind::Optional) { 
        type = type->Cast<TOptionalExprType>()->GetItemType(); 
    } 
    argumentDataSlot = type->Cast<TDataExprType>()->GetSlot(); 
 
    // If jsonExpr has JsonDocument type, there is no need to parse it 
    if (argumentDataSlot == EDataSlot::JsonDocument) { 
        return jsonExpr; 
    } 
 
    // Otherwise jsonExpr has Json type and we need to wrap it in Json2::Parse 
    return BuildJsonParse(jsonExpr, ctx); 
} 
 
TExprNode::TPtr GetJsonDocumentOrParseJson(const TCoJsonQueryBase& jsonExpr, TExprContext& ctx, EDataSlot& argumentDataSlot) { 
    return GetJsonDocumentOrParseJson(jsonExpr.Json().Ptr(), ctx, argumentDataSlot); 
} 
 
TExprNode::TPtr BuildJsonSerialize(const TExprNode::TPtr& resourceExpr, TExprContext& ctx) { 
    auto resourcePos = resourceExpr->Pos(); 
 
    auto argumentsType = ctx.MakeType<TTupleExprType>(TTypeAnnotationNode::TListType{ 
        ctx.MakeType<TOptionalExprType>(ctx.MakeType<TResourceExprType>("JsonNode")), 
    }); 
 
    auto udfArgumentsType = ctx.MakeType<TTupleExprType>(TTypeAnnotationNode::TListType{ 
        argumentsType, 
        ctx.MakeType<TStructExprType>(TVector<const TItemExprType*>{}), 
        ctx.MakeType<TTupleExprType>(TTypeAnnotationNode::TListType{}) 
    }); 
 
    auto parse = Build<TCoUdf>(ctx, resourcePos) 
        .MethodName() 
            .Build("Json2.Serialize") 
        .RunConfigValue<TCoVoid>() 
            .Build() 
        .UserType(ExpandType(resourcePos, *udfArgumentsType, ctx)) 
        .Done().Ptr(); 
 
    return Build<TCoApply>(ctx, resourcePos) 
        .Callable(parse) 
        .FreeArgs() 
            .Add(resourceExpr) 
            .Build() 
        .Done().Ptr(); 
} 
 
TExprNode::TPtr BuildJsonCompilePath(const TCoJsonQueryBase& jsonExpr, TExprContext& ctx) { 
    auto jsonPathPos = jsonExpr.JsonPath().Pos(); 
 
    auto argumentsType = ctx.MakeType<TTupleExprType>(TTypeAnnotationNode::TListType{ 
        ctx.MakeType<TDataExprType>(EDataSlot::Utf8) 
    }); 
 
    auto udfArgumentsType = ctx.MakeType<TTupleExprType>(TTypeAnnotationNode::TListType{ 
        argumentsType, 
        ctx.MakeType<TStructExprType>(TVector<const TItemExprType*>{}), 
        ctx.MakeType<TTupleExprType>(TTypeAnnotationNode::TListType{}) 
    }); 
 
    auto compilePath = Build<TCoUdf>(ctx, jsonPathPos) 
        .MethodName() 
            .Build("Json2.CompilePath") 
        .RunConfigValue<TCoVoid>() 
            .Build() 
        .UserType(ExpandType(jsonPathPos, *udfArgumentsType, ctx)) 
        .Done().Ptr(); 
 
    return Build<TCoApply>(ctx, jsonPathPos) 
        .Callable(compilePath) 
        .FreeArgs() 
            .Add(jsonExpr.JsonPath()) 
            .Build() 
        .Done().Ptr(); 
} 
 
template<bool Ordered>
TExprNode::TPtr CanonizeMultiMap(const TExprNode::TPtr& node, TExprContext& ctx) {
    YQL_CLOG(DEBUG, Core) << "Canonize " << node->Content() << " of width " << node->Tail().ChildrenSize() - 1U;
    return ctx.Builder(node->Pos())
        .Callable(Ordered ? "OrderedFlatMap" : "FlatMap")
            .Add(0, node->HeadPtr())
            .Add(1, ctx.DeepCopyLambda(node->Tail(), {ctx.NewCallable(node->Tail().Pos(), "AsList", GetLambdaBody(node->Tail()))}))
        .Seal().Build();
}

template<bool Not>
TExprNode::TPtr OptimizeDistinctFrom(const TExprNode::TPtr& node, TExprContext& ctx) {
    const auto leftType = node->Head().GetTypeAnn();
    const auto rightType = node->Tail().GetTypeAnn();
    if (IsSameAnnotation(*leftType, *rightType)) {
        YQL_CLOG(DEBUG, Core) << node->Content() << " with arguments of same type";
        return ctx.RenameNode(*node, Not ? "AggrEquals" :  "AggrNotEquals");
    }

    if (CanCompare<true>(leftType, rightType) == ECompareOptions::Comparable) {
        YQL_CLOG(DEBUG, Core) << node->Content() << " with non-Optional arguments";
        return ctx.RenameNode(*node, Not ? "==" : "!=");
    }

    if (leftType->GetKind() == ETypeAnnotationKind::Null && rightType->GetKind() != ETypeAnnotationKind::Optional ||
        rightType->GetKind() == ETypeAnnotationKind::Null && leftType->GetKind() != ETypeAnnotationKind::Optional) {
        YQL_CLOG(DEBUG, Core) << node->Content() << " with Null and non-Optional args";
        return MakeBool<!Not>(node->Pos(), ctx);
    }
    return node;
}

template<bool ByPrefix>
TExprNode::TPtr ExpandSelectMembers(const TExprNode::TPtr& node, TExprContext& ctx) {
    std::set<std::string_view> prefixes;
    node->Child(1)->ForEachChild([&](const TExprNode& prefixNode){ prefixes.emplace(prefixNode.Content()); });

    const MemberUpdaterFunc filterByPrefixFunc = [&prefixes](const std::string_view& memberName, const TTypeAnnotationNode*) {
        if constexpr (ByPrefix)
            return std::any_of(prefixes.cbegin(), prefixes.cend(), [&memberName](const std::string_view& prefix){ return memberName.starts_with(prefix); });
        else
            return prefixes.contains(memberName);
    };
    TExprNode::TListType members;
    UpdateStructMembers(ctx, node->HeadPtr(), ByPrefix ? "SelectMembers" : "FilterMembers", members, filterByPrefixFunc);
    return ctx.NewCallable(node->Pos(), "AsStruct", std::move(members));
}

void RegisterCoSimpleCallables1(TCallableOptimizerMap& map) {
    using namespace std::placeholders;

    map["SafeCast"] = std::bind(&OptimizeCast<false>, _1, _2);
    map["StrictCast"] = std::bind(&OptimizeCast<true>, _1, _2);

    map["AuthTokens"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) -> TExprNode::TPtr {
        YQL_CLOG(DEBUG, Core) << "AuthTokensResult";

        auto result = ctx.Builder(node->Pos());
        auto cListBuilder = result.Callable("List");
        auto& listBuilder = cListBuilder.Add(0U, ExpandType(node->Pos(), *node->GetTypeAnn(), ctx));

        const auto structType = ExpandType(node->Pos(), *node->GetTypeAnn()->Cast<TListExprType>()->GetItemType(), ctx);
        ui32 i = 0U;
        for (const auto& data : optCtx.Types->Credentials)
            for (const auto& cred : *data) {
                listBuilder.Callable(++i, "Struct")
                    .Add(0U, structType)
                    .List(1U)
                        .Atom(0U, "Name")
                        .Callable(1U, "String")
                            .Atom(0U, cred.first)
                        .Seal()
                    .Seal()
                    .List(2U)
                        .Atom(0U, "Category")
                        .Callable(1U, "String")
                            .Atom(0U, cred.second.Category)
                        .Seal()
                    .Seal()
                    .List(3U)
                        .Atom(0U, "Subcategory")
                        .Callable(1U, "String")
                            .Atom(0U, cred.second.Subcategory)
                        .Seal()
                    .Seal()
                .Seal();
            }
        listBuilder.Seal();

        return result.Build();
    };

    map["Files"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) {
        YQL_CLOG(DEBUG, Core) << "FilesResult";

        auto result = ctx.Builder(node->Pos());
        auto cListBuilder = result.Callable("List");
        auto& listBuilder = cListBuilder.Add(0U, ExpandType(node->Pos(), *node->GetTypeAnn(), ctx));

        const auto structType = ExpandType(node->Pos(), *node->GetTypeAnn()->Cast<TListExprType>()->GetItemType(), ctx);
        const auto structure = node->GetTypeAnn()->Cast<TListExprType>()->GetItemType()->Cast<TStructExprType>();
        const auto urlType = ExpandType(node->Pos(), *structure->GetItems()[structure->FindItem("Url").GetRef()]->GetItemType(), ctx);
        const auto pathType = ExpandType(node->Pos(), *structure->GetItems()[structure->FindItem("Path").GetRef()]->GetItemType(), ctx);

        const auto& items = optCtx.Types->UserDataStorage->GetDirectoryContent(node->Head().Content());
        ui32 i = 0U;
        for (const auto& item : items) {
            listBuilder.Callable(++i, "Struct")
                .Add(0U, structType)
                .List(1U)
                    .Atom(0U, "Name")
                    .Callable(1U, "String")
                        .Atom(0U, item.first)
                    .Seal()
                .Seal()
                .List(2U)
                    .Atom(0U, "IsFolder")
                    .Callable(1U, "Bool")
                        .Atom(0U, item.second ? "false" : "true", TNodeFlags::Default)
                    .Seal()
                .Seal()
                .List(3U)
                    .Atom(0U, "Url")
                    .Do([&item, &urlType](auto& b) -> decltype(b) {
                        return item.second && EUserDataType::URL == item.second->Type
                            ? (
                                b.Callable(1U, "Just")
                                    .Callable(0U, "String")
                                        .Atom(0U, item.second->Data)
                                    .Seal()
                                .Seal()
                            )
                            : (
                                b.Callable(1U, "Nothing")
                                    .Add(0U, urlType)
                                .Seal()
                            );
                    })
                .Seal()
                .List(4U)
                    .Atom(0U, "Path")
                    .Do([&item, &pathType](auto& b) -> decltype(b) {
                        return item.second && EUserDataType::PATH == item.second->Type
                            ? (
                                b.Callable(1U, "Just")
                                    .Callable(0U, "String")
                                        .Atom(0U, item.second->Data)
                                    .Seal()
                                .Seal()
                            )
                            : (
                                b.Callable(1U, "Nothing")
                                    .Add(0U, pathType)
                                .Seal()
                            );
                    })
                .Seal()
            .Seal();
        }
        listBuilder.Seal();

        return result.Build();
    };

    map["ToFlow"] = std::bind(&OptimizeToFlow, _1, _2);

    map["Collect"] = std::bind(&OptimizeCollect, _1, _2);
    map["LazyList"] = std::bind(&DropDuplicate, _1, _2);

    map["FlatMap"] = std::bind(&SimpleFlatMap<false>, _1, _2, _3);
    map["OrderedFlatMap"] = std::bind(&SimpleFlatMap<true>, _1, _2, _3);

    map["MultiMap"] = std::bind(&CanonizeMultiMap<false>, _1, _2);
    map["OrderedMultiMap"] = std::bind(&CanonizeMultiMap<true>, _1, _2);

    map["LMap"] = map["OrderedLMap"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) {
        if (CanRewriteToEmptyContainer(*node)) {
            const auto& inputToCheck = SkipCallables(node->Head(), SkippableCallables);
            if (IsEmptyContainer(inputToCheck) || IsEmpty(inputToCheck, *optCtx.Types)) {
                YQL_CLOG(DEBUG, Core) << "Empty " << node->Content() << " over " << inputToCheck.Content();
                auto res = ctx.NewCallable(inputToCheck.Pos(), GetEmptyCollectionName(node->GetTypeAnn()), {ExpandType(node->Pos(), *node->GetTypeAnn(), ctx)});
                return KeepConstraints(res, *node, ctx);
            }

            const auto& lambdaRootToCheck = SkipCallables(node->Tail().Tail(), SkippableCallables);
            if (IsEmptyContainer(lambdaRootToCheck) || IsEmpty(lambdaRootToCheck, *optCtx.Types)) {
                YQL_CLOG(DEBUG, Core) << "Empty " << node->Content() << " with " << lambdaRootToCheck.Content();
                auto res = ctx.NewCallable(lambdaRootToCheck.Pos(), GetEmptyCollectionName(node->GetTypeAnn()), {ExpandType(node->Pos(), *node->GetTypeAnn(), ctx)});
                return KeepConstraints(res, *node, ctx);
            }
        }

        return node;
    };

    map["FlatMapToEquiJoin"] = map["OrderedFlatMapToEquiJoin"] =
        [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext&) {
            const auto self = TCoFlatMapToEquiJoinBase(node);
            return ConvertSqlInPredicatesToJoins(self, ctx);
    };

    map["SkipNullMembers"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        const auto skipNullMembers = TCoSkipNullMembers(node);
        if (!skipNullMembers.Members()) {
            return node;
        }

        if (const auto maybeInnerSkip = skipNullMembers.Input().Maybe<TCoSkipNullMembers>()) {
            const auto innerSkip = maybeInnerSkip.Cast();

            if (!innerSkip.Members()) {
                return node;
            }

            TSet<TStringBuf> members;

            for (const auto& member : skipNullMembers.Members().Cast()) {
                members.insert(member.Value());
            }

            for (const auto& member : innerSkip.Members().Cast()) {
                members.insert(member.Value());
            }

            TExprNode::TListType membersList;
            for (const auto& memberName : members) {
                membersList.push_back(ctx.NewAtom(innerSkip.Pos(), memberName));
            }

            YQL_CLOG(DEBUG, Core) << "FuseSkipNullMembers";
            return Build<TCoSkipNullMembers>(ctx, innerSkip.Pos())
                .Input(innerSkip.Input())
                .Members()
                    .Add(membersList)
                    .Build()
                .Done()
                .Ptr();
        }

        return node;
    };

    map["SkipNullElements"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        const auto skipNullElements = TCoSkipNullElements(node);
        if (!skipNullElements.Elements()) {
            return node;
        }

        if (const auto maybeInnerSkip = skipNullElements.Input().Maybe<TCoSkipNullElements>()) {
            const auto innerSkip = maybeInnerSkip.Cast();

            if (!innerSkip.Elements()) {
                return node;
            }

            TSet<TStringBuf> elements;

            for (const auto& element : skipNullElements.Elements().Cast()) {
                elements.emplace(element.Value());
            }

            for (const auto& element : innerSkip.Elements().Cast()) {
                elements.emplace(element.Value());
            }

            TExprNode::TListType elementsList;
            for (const auto& elementIndex : elements) {
                elementsList.emplace_back(ctx.NewAtom(innerSkip.Pos(), elementIndex));
            }

            YQL_CLOG(DEBUG, Core) << "FuseSkipNullElements";
            return Build<TCoSkipNullElements>(ctx, innerSkip.Pos())
                .Input(innerSkip.Input())
                .Elements()
                    .Add(elementsList)
                    .Build()
                .Done()
                .Ptr();
        }

        return node;
    };

    map["Filter"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) {
        YQL_CLOG(DEBUG, Core) << "Canonize " << node->Content();
        return ConvertFilterToFlatmap<TCoFilter, TCoFlatMap>(TCoFilter(node), ctx, optCtx);
    };

    map["OrderedFilter"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) {
        YQL_CLOG(DEBUG, Core) << "Canonize " << node->Content();
        return ConvertFilterToFlatmap<TCoOrderedFilter, TCoOrderedFlatMap>(TCoOrderedFilter(node), ctx, optCtx);
    };

    map["Map"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        YQL_CLOG(DEBUG, Core) << "Canonize " << node->Content();
        return ConvertMapToFlatmap<TCoMap, TCoFlatMap>(TCoMap(node), ctx);
    };

    map["OrderedMap"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        YQL_CLOG(DEBUG, Core) << "Canonize " << node->Content();
        return ConvertMapToFlatmap<TCoOrderedMap, TCoOrderedFlatMap>(TCoOrderedMap(node), ctx);
    };

    map["ExtractMembers"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        if (IsSameAnnotation(*node->GetTypeAnn(), *node->Head().GetTypeAnn())) {
            YQL_CLOG(DEBUG, Core) << "Drop redundant ExtractMembers over " << node->Head().Content();
            return node->HeadPtr();
        }

        if (node->Head().IsCallable(node->Content())) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
            return ctx.ChangeChild(*node, 0U, node->Head().HeadPtr());
        }

        if (node->Head().IsCallable({"Nothing", "List"}) && 1U == node->Head().ChildrenSize()) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
            return ctx.ChangeChild(node->Head(), 0U, ExpandType(node->Pos(), *node->GetTypeAnn(), ctx));
        }

        if (node->Head().IsCallable({"Just", "AsList"})) {
            YQL_CLOG(DEBUG, Core) << "Move " << node->Content() << " over " << node->Head().Content();
            TSet<TString> fields;
            node->Tail().ForEachChild([&fields](const TExprNode& child) {
                fields.emplace(child.Content());
            });

            auto args = node->Head().ChildrenList();
            for (auto& arg : args) {
                arg = FilterByFields(node->Pos(), arg, fields, ctx, true);
            }

            return ctx.ChangeChildren(node->Head(), std::move(args));
        }

        if (node->Head().IsCallable("AssumeAllMembersNullableAtOnce")) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
            return ctx.SwapWithHead(*node);
        }

        return node;
    };

    map["Lookup"] = std::bind(&OptimizeContains<false, true>, _1, _2);
    map["Contains"] = std::bind(&OptimizeContains<false>, _1, _2);
    map["ListHas"] = std::bind(&OptimizeContains<true>, _1, _2);
    map["SqlIn"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext&) {
        auto collection = node->HeadPtr();
        auto lookup = node->ChildPtr(1);
        auto options = node->ChildPtr(2);

        auto collectionType = collection->GetTypeAnn();
        auto collectionKind = collectionType->GetKind();

        if (collectionKind == ETypeAnnotationKind::Null) {
            YQL_CLOG(DEBUG, Core) << "IN Null";
            return MakeBoolNothing(node->Pos(), ctx);
        }

        if (collectionKind == ETypeAnnotationKind::Optional) {
            YQL_CLOG(DEBUG, Core) << "IN Optional";

            return ctx.Builder(node->Pos())
                .Callable("FlatMap")
                    .Add(0, collection)
                    .Lambda(1)
                        .Param("collection")
                        .Callable("MatchType")
                            .Callable(0, "SqlIn")
                                .Arg(0, "collection")
                                .Add(1, lookup)
                                .Add(2, options)
                            .Seal()
                            .Atom(1, "Optional", TNodeFlags::Default)
                            .Lambda(2)
                                .Param("input")
                                .Arg("input")
                            .Seal()
                            .Lambda(3)
                                .Param("input")
                                .Callable("Just")
                                    .Arg(0, "input")
                                .Seal()
                            .Seal()
                        .Seal()
                    .Seal()
                .Seal()
                .Build();
        }

        const bool isAnsi = HasSetting(*options, "ansi");
        if (collectionKind == ETypeAnnotationKind::EmptyDict ||
            collectionKind == ETypeAnnotationKind::EmptyList ||
            (
                collectionKind == ETypeAnnotationKind::Tuple &&
                collectionType->Cast<TTupleExprType>()->GetSize() == 0
            ))
        {
            if (!isAnsi) {
                // legacy IN: null in () should equals null
                if (lookup->GetTypeAnn()->HasOptionalOrNull()) {
                    YQL_CLOG(DEBUG, Core) << "NULL IN legacy";
                    return ctx.Builder(node->Pos())
                        .Callable("If")
                            .Callable(0, "HasNull")
                                .Add(0, lookup)
                            .Seal()
                            .Callable(1, "Null")
                            .Seal()
                            .Add(2, MakeBool(node->Pos(), false, ctx))
                        .Seal()
                        .Build();
                }
                auto lookupTypeNoOpt = RemoveAllOptionals(lookup->GetTypeAnn());
                if (lookupTypeNoOpt->GetKind() == ETypeAnnotationKind::Null) {
                    return MakeBoolNothing(node->Pos(), ctx);
                }
            }

            YQL_CLOG(DEBUG, Core) << "IN Empty collection";
            return (node->GetTypeAnn()->GetKind() == ETypeAnnotationKind::Optional) ?
                   MakeOptionalBool(node->Pos(), false, ctx) : MakeBool(node->Pos(), false, ctx);
        }

        if (collectionKind == ETypeAnnotationKind::Tuple) {
            auto tupleType = collectionType->Cast<TTupleExprType>();
            YQL_ENSURE(tupleType->GetSize());
            auto firstItemType = tupleType->GetItems().front();

            bool tupleElementsHaveSameType =
                AllOf(tupleType->GetItems(),
                      [&](const TTypeAnnotationNode* item) {
                          return IsSameAnnotation(*firstItemType, *item);
                      });

            if (!tupleElementsHaveSameType) {
                YQL_CLOG(DEBUG, Core) << "IN heterogeneous tuple";
                auto collections = DeduplicateAndSplitTupleCollectionByTypes(*collection, ctx);
                YQL_ENSURE(collections.size() > 1);

                TExprNodeList predicates;
                predicates.reserve(collections.size());

                for (auto& splittedCollection : collections) {
                    predicates.push_back(ctx.NewCallable(node->Pos(), "SqlIn", { splittedCollection, lookup, options}));
                }

                return ctx.NewCallable(node->Pos(), "Or", std::move(predicates));
            }
        }

        if (isAnsi) {
            auto lookupTypeNoOpt = RemoveAllOptionals(lookup->GetTypeAnn());
            if (lookupTypeNoOpt->GetKind() == ETypeAnnotationKind::Null) {
                YQL_CLOG(DEBUG, Core) << "NULL IN";
                return ctx.Builder(node->Pos())
                    .Callable("If")
                        .Add(0, BuildSqlInCollectionEmptyPred(TCoSqlIn(node), ctx))
                        .Add(1, MakeBool(node->Pos(), false, ctx))
                        .Callable(2, "Null")
                        .Seal()
                    .Seal()
                    .Build();
            }
        }

        return node;
    };

    map["DictItems"] = std::bind(&OptimizeDictItems, _1, _2);
    map["DictKeys"] = std::bind(&OptimizeDictItems, _1, _2);
    map["DictPayloads"] = std::bind(&OptimizeDictItems, _1, _2);

    map["ListIf"] = std::bind(&OptimizeContainerIf<true>, _1, _2);
    map["OptionalIf"] = std::bind(&OptimizeContainerIf<false>, _1, _2);

    map["FlatListIf"] = std::bind(&OptimizeFlatContainerIf<true>, _1, _2);
    map["FlatOptionalIf"] = std::bind(&OptimizeFlatContainerIf<false>, _1, _2);

    map["Skip"] = [](const TExprNode::TPtr& node, TExprContext& /*ctx*/, TOptimizeContext& /*optCtx*/) {
        if (node->Tail().IsCallable("Uint64")) {
            const auto value = FromString<ui64>(node->Tail().Head().Content());
            if (!value) {
                YQL_CLOG(DEBUG, Core) << node->Content() << " with " << node->Tail().Content() << " '" << node->Tail().Head().Content();
                return node->HeadPtr();
            }
        }

        return node;
    };

    map["Take"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) {
        if (node->Tail().IsCallable("Uint64")) {
            const auto value = FromString<ui64>(node->Tail().Head().Content());
            if (!value) {
                YQL_CLOG(DEBUG, Core) << node->Content() << " with " << node->Tail().Content() << " '" << node->Tail().Head().Content();
                auto res = ctx.NewCallable(node->Tail().Pos(), GetEmptyCollectionName(node->GetTypeAnn()), {ExpandType(node->Pos(), *node->GetTypeAnn(), ctx)});
                res = KeepConstraints(res, *node, ctx);
                return KeepColumnOrder(res, *node, ctx, *optCtx.Types);
            }
        }

        return node;
    };

    map["TakeWhile"] = std::bind(&OptimizeWhile<true>, _1, _2);
    map["SkipWhile"] = std::bind(&OptimizeWhile<false>, _1, _2);

    map["TakeWhileInclusive"] = std::bind(&OptimizeWhile<true, true>, _1, _2);
    map["SkipWhileInclusive"] = std::bind(&OptimizeWhile<false, true>, _1, _2);

    map[TCoExtend::CallableName()] = map[TCoOrderedExtend::CallableName()] = map[TCoMerge::CallableName()] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) {
        if (node->ChildrenSize() == 1) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over one child";
            return node->HeadPtr();
        }

        for (ui32 i = 0; i < node->ChildrenSize(); ++i) {
            auto& child = SkipCallables(*node->Child(i), SkippableCallables);
            if (IsEmptyContainer(child) || IsEmpty(child, *optCtx.Types)) {
                YQL_CLOG(DEBUG, Core) << node->Content() << " over empty list";
                if (node->ChildrenSize() == 2) {
                    return KeepConstraints(node->ChildPtr(1 - i), *node, ctx);
                }

                TExprNode::TListType newChildren = node->ChildrenList();
                newChildren.erase(newChildren.begin() + i);
                return KeepConstraints(ctx.ChangeChildren(*node, std::move(newChildren)), *node, ctx);
            }

            if (TCoExtendBase::Match(node->Child(i))) {
                TExprNode::TListType newChildren = node->ChildrenList();
                TExprNode::TListType insertedChildren = node->Child(i)->ChildrenList();
                newChildren.erase(newChildren.begin() + i);
                newChildren.insert(newChildren.begin() + i, insertedChildren.begin(), insertedChildren.end());
                return ctx.ChangeChildren(*node, std::move(newChildren));
            }
        }

        for (ui32 i = 0; i < node->ChildrenSize() - 1; ++i) {
            if (node->Child(i)->IsCallable("AsList") && node->Child(i + 1)->IsCallable("AsList")) {
                YQL_CLOG(DEBUG, Core) << node->Content() << " over 2 or more AsList";
                ui32 j = i + 2;
                for (; j < node->ChildrenSize(); ++j) {
                    if (!node->Child(j)->IsCallable("AsList")) {
                        break;
                    }
                }

                // fuse [i..j)
                TExprNode::TListType fusedChildren;
                for (ui32 listIndex = i; listIndex < j; ++listIndex) {
                    fusedChildren.insert(fusedChildren.end(), node->Child(listIndex)->Children().begin(), node->Child(listIndex)->Children().end());
                }

                auto fused = ctx.ChangeChildren(*node->Child(i), std::move(fusedChildren));
                if (j - i == node->ChildrenSize()) {
                    return fused;
                }

                TExprNode::TListType newChildren = node->ChildrenList();
                newChildren.erase(newChildren.begin() + i + 1, newChildren.begin() + j);
                newChildren[i] = fused;
                return ctx.ChangeChildren(*node, std::move(newChildren));
            }
        }

        return node;
    };

    map["ForwardList"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        if (node->Head().IsCallable("Iterator")) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
            return node->Head().HeadPtr();
        }

        if (node->Head().IsCallable("ToFlow")) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
            return ctx.WrapByCallableIf(ETypeAnnotationKind::Stream == node->Head().Head().GetTypeAnn()->GetKind(), node->Content(), node->Head().HeadPtr());
        }

        return node;
    };

    map["Iterator"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        if (node->ChildrenSize() == 1 && node->Head().IsCallable("ForwardList")) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
            return ctx.WrapByCallableIf(ETypeAnnotationKind::Flow == node->Head().Head().GetTypeAnn()->GetKind(), "FromFlow", node->Head().HeadPtr());
        }

        return node;
    };

    map["Length"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        const auto& nodeToCheck = SkipCallables(node->Head(), SkippableCallables);
        if (nodeToCheck.IsCallable("AsList")) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << nodeToCheck.Content();
            return ctx.NewCallable(node->Pos(), "Uint64",
                { ctx.NewAtom(node->Pos(), ToString(nodeToCheck.ChildrenSize()), TNodeFlags::Default) });
        }

        if (nodeToCheck.IsCallable("List")) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << nodeToCheck.Content();
            return ctx.NewCallable(node->Pos(), "Uint64",
                { ctx.NewAtom(node->Pos(), ToString(nodeToCheck.ChildrenSize() - 1), TNodeFlags::Default) });
        }

        if (IsListReorder(nodeToCheck) || nodeToCheck.IsCallable(
            {"CalcOverWindow", "CalcOverSessionWindow", "CalcOverWindowGroup", "Chain1Map", "FoldMap", "Fold1Map"}))
        {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << nodeToCheck.Content();
            return ctx.ChangeChild(*node, 0U, nodeToCheck.HeadPtr());
        }

        if (nodeToCheck.IsCallable({"FlatMap", "OrderedFlatMap"})
            && nodeToCheck.Head().GetTypeAnn()->GetKind() == ETypeAnnotationKind::List
            && IsJustOrSingleAsList(nodeToCheck.Tail().Tail())) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << nodeToCheck.Content();
            return ctx.ChangeChild(*node, 0U, nodeToCheck.HeadPtr());
        }

        if (nodeToCheck.IsCallable("Take") && nodeToCheck.Head().IsCallable({"ForwardList", "Collect"})) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << nodeToCheck.Content();
            return ctx.Builder(node->Pos())
                .Callable("Min")
                    .Add(0, nodeToCheck.TailPtr())
                    .Callable(1, "Length")
                        .Add(0, nodeToCheck.HeadPtr())
                    .Seal()
                .Seal()
                .Build();
        }

        if (nodeToCheck.IsCallable("Skip") && nodeToCheck.Head().IsCallable({"ForwardList", "Collect"})) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << nodeToCheck.Content();
            auto fullLen = ctx.NewCallable(node->Pos(), "Length", { nodeToCheck.HeadPtr() });
            return ctx.Builder(node->Pos())
                .Callable("-")
                    .Add(0, fullLen)
                    .Callable(1, "Min")
                        .Add(0, nodeToCheck.TailPtr())
                        .Add(1, fullLen)
                    .Seal()
                .Seal()
                .Build();
        }

        return node;
    };

    map["HasItems"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        if (node->Head().IsCallable({"Append", "Insert", "Prepend"})) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
            return MakeBool<true>(node->Pos(), ctx);
        }

        const auto& nodeToCheck = SkipCallables(node->Head(), SkippableCallables);
        if (nodeToCheck.IsCallable({"AsList","AsDict"})) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << nodeToCheck.Content();
            return MakeBool(node->Pos(), nodeToCheck.ChildrenSize() > 0U, ctx);
        }

        if (nodeToCheck.IsCallable({"List","Dict"})) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << nodeToCheck.Content();
            return MakeBool(node->Pos(), nodeToCheck.ChildrenSize() > 1U, ctx);
        }

        if (IsListReorder(nodeToCheck) || nodeToCheck.IsCallable(
            {"CalcOverWindow", "CalcOverSessionWindow", "CalcOverWindowGroup", "Chain1Map", "FoldMap", "Fold1Map"}))
        {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << nodeToCheck.Content();
            return ctx.ChangeChild(*node, 0U, nodeToCheck.HeadPtr());
        }

        if (nodeToCheck.IsCallable({"FlatMap", "OrderedFlatMap"})
            && nodeToCheck.Head().GetTypeAnn()->GetKind() == ETypeAnnotationKind::List
            && IsJustOrSingleAsList(nodeToCheck.Tail().Tail())) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << nodeToCheck.Content();
            return ctx.ChangeChild(*node, 0U, nodeToCheck.HeadPtr());
        }

        if (nodeToCheck.IsCallable("Take") && nodeToCheck.Head().IsCallable({"ForwardList", "Collect"})) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << nodeToCheck.Content();
            return ctx.Builder(node->Pos())
                .Callable("If")
                    .Callable(0, "==")
                        .Callable(0, "Uint64")
                            .Atom(0, "0", TNodeFlags::Default)
                        .Seal()
                        .Add(1, nodeToCheck.TailPtr())
                    .Seal()
                    .Add(1, MakeBool<false>(node->Pos(), ctx))
                    .Callable(2, "HasItems")
                        .Add(0, nodeToCheck.HeadPtr())
                    .Seal()
                .Seal()
                .Build();
        }

        if (nodeToCheck.IsCallable("Skip") && nodeToCheck.Head().IsCallable({"ForwardList", "Collect"})) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << nodeToCheck.Content();
            return ctx.Builder(node->Pos())
                .Callable(">")
                    .Callable(0, "Length")
                        .Add(0, nodeToCheck.HeadPtr())
                    .Seal()
                    .Add(1, nodeToCheck.TailPtr())
                .Seal()
                .Build();
        }

        return node;
    };

    map["Struct"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        YQL_CLOG(DEBUG, Core) << "ConvertStructToAsStruct";

        TExprNode::TListType asStructChildren(node->ChildrenList());
        if (node->ChildrenSize() > 0) {
            asStructChildren.erase(asStructChildren.cbegin());
        }

        return ctx.NewCallable(node->Pos(), "AsStruct", std::move(asStructChildren));
    };

    map["Member"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        if (node->Head().IsCallable("AsStruct")) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
            return ExtractMember(*node);
        }

        if (node->Head().IsCallable("Just")) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
            auto ret = ctx.ChangeChild(*node, 0U, node->Head().HeadPtr());
            const auto structType = node->Head().Head().GetTypeAnn()->Cast<TStructExprType>();
            const auto memberType = structType->GetItems()[*structType->FindItem(node->Tail().Content())]->GetItemType();
            return ctx.WrapByCallableIf(!memberType->IsOptionalOrNull(), "Just", std::move(ret));
        }

        if (node->Head().IsCallable("Nothing")) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
            return ctx.ChangeChild(node->Head(), 0U, ExpandType(node->Pos(), *node->GetTypeAnn(), ctx));
        }

        if (node->Head().IsCallable("ExtractMembers")) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
            return ctx.ChangeChild(*node, 0, node->Head().HeadPtr());
        }

        return node;
    };

    map["RemoveMember"] = std::bind(&ExpandRemoveMember, _1, _2);
    map["ForceRemoveMember"] = std::bind(&ExpandRemoveMember, _1, _2);
    map["FlattenMembers"] = std::bind(&ExpandFlattenMembers, _1, _2);
    map["FlattenStructs"] = std::bind(&ExpandFlattenStructs, _1, _2);
    map["SelectMembers"] = std::bind(&ExpandSelectMembers<true>, _1, _2);
    map["FilterMembers"] = std::bind(&ExpandSelectMembers<false>, _1, _2);
    map["DivePrefixMembers"] = std::bind(&ExpandDivePrefixMembers, _1, _2);
    map["AddMember"] = std::bind(&ExpandAddMember, _1, _2);
    map["ReplaceMember"] = std::bind(&ExpandReplaceMember, _1, _2);

    map["RemovePrefixMembers"] = std::bind(&ExpandRemovePrefixMembers, _1, _2);

    map["FlattenByColumns"] = std::bind(&ExpandFlattenByColumns, _1, _2);

    map["AsStruct"] = std::bind(&OptimizeAsStruct, _1, _2);

    map["Nth"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        if (node->Head().Type() == TExprNode::List) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over tuple literal";
            const auto index = FromString<ui32>(node->Tail().Content());
            return node->Head().ChildPtr(index);
        }

        if (node->Head().IsCallable("Just")) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
            auto ret = ctx.ChangeChild(*node, 0U, node->Head().HeadPtr());
            const auto tupleType = node->Head().Head().GetTypeAnn()->Cast<TTupleExprType>();
            const auto elemType = tupleType->GetItems()[FromString<ui32>(node->Tail().Content())];
            return ctx.WrapByCallableIf(elemType->GetKind() != ETypeAnnotationKind::Optional, "Just", std::move(ret));
        }

        if (node->Head().IsCallable("Nothing")) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
            return ctx.ChangeChild(node->Head(), 0U, ExpandType(node->Pos(), *node->GetTypeAnn(), ctx));
        }

        return node;
    };

    map["ToString"] = std::bind(&RemoveToStringFromString, _1);

    map["Coalesce"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        if (node->Head().IsCallable("Nothing")) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
            return RemoveNothingFromCoalesce(*node, ctx);
        }

        if (node->Head().IsCallable("Just")) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
            if (IsSameAnnotation(*node->Head().GetTypeAnn(), *node->Child(node->ChildrenSize() - 1)->GetTypeAnn())) {
                return node->HeadPtr();
            } else {
                return node->Head().HeadPtr();
            }
        }

        if (const auto& input = node->Head(); IsTransparentIfPresent(input)) {
            if (auto lambda = IsSameAnnotation(*input.GetTypeAnn()->Cast<TOptionalExprType>()->GetItemType(), *node->Tail().GetTypeAnn()) ?
                ctx.DeepCopyLambda(*input.Child(1), input.Child(1)->Tail().HeadPtr()) :
                IsSameAnnotation(*input.GetTypeAnn(), *node->Tail().GetTypeAnn()) ? input.ChildPtr(1) : nullptr) {

                YQL_CLOG(DEBUG, Core) << node->Content() << " over transparent " << input.Content();
                return ctx.Builder(node->Pos())
                    .Callable("IfPresent")
                        .Add(0, input.HeadPtr())
                        .Add(1, std::move(lambda))
                        .Add(2, node->TailPtr())
                    .Seal().Build();
            }
        }

        if (node->Tail().IsCallable("Bool")) {
            return PropagateCoalesceWithConstIntoLogicalOps(node, ctx);
        }

        return node;
    };

    map["Exists"] = std::bind(&OptimizeExists, _1, _2);

    map["Convert"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        if (node->Head().GetTypeAnn()->GetKind() == ETypeAnnotationKind::Data) {
            const auto targetType = node->GetTypeAnn()->Cast<TDataExprType>();
            if (node->Head().IsCallable("Bool") && IsDataTypeNumeric(targetType->GetSlot())) {
                YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content() << " '" << node->Head().Head().Content();
                return ctx.NewCallable(node->Pos(), targetType->GetName(),
                    {ctx.NewAtom(node->Pos(), FromString<bool>(node->Head().Head().Content()) ? "1" : "0", TNodeFlags::Default)});
            }

            if (const auto maybeInt = TMaybeNode<TCoIntegralCtor>(&node->Head())) {
                TString atomValue;
                if (AllowIntegralConversion(maybeInt.Cast(), false, targetType->GetSlot(), &atomValue)) {
                    YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content() << " '" << node->Head().Head().Content();
                    return ctx.NewCallable(node->Pos(), targetType->GetName(),
                        {ctx.NewAtom(node->Pos(), atomValue, TNodeFlags::Default)});
                }
            }
        }

        return node;
    };

    map[IfName] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        if (node->Child(1)->IsCallable("Bool")) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " with literal predicate";
            const auto value = FromString<bool>(node->Child(1)->Head().Content());
            return ctx.NewCallable(node->Pos(), SyncName, { node->HeadPtr(), node->ChildPtr(value ? 2 : 3) });
        }

        return node;
    };

    map["If"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        for (auto i = 0U; i < node->ChildrenSize() - 1U; ++++i) {
            if (node->Child(i)->IsCallable("Bool")) {
                YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Child(i)->Content() << " '" << node->Child(i)->Head().Content();
                auto children = node->ChildrenList();
                if (FromString<bool>(children[i]->Head().Content())) {
                    const auto last = i;
                    children[last] = std::move(children[++i]);
                    children.resize(i);
                } else {
                    auto it = children.cbegin();
                    std::advance(it, i);
                    children.erase(it, it + 2U);
                 }
                 return children.size() > 1U ? ctx.ChangeChildren(*node, std::move(children)) : children.front();
            }
        }

        if (const auto lastPredicateIndex = node->ChildrenSize() - 3U; node->Child(lastPredicateIndex)->IsCallable("Not")) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Child(lastPredicateIndex)->Content();
            auto children = node->ChildrenList();
            children[lastPredicateIndex] = children[lastPredicateIndex]->HeadPtr();
            std::swap(children[lastPredicateIndex + 1U], children[lastPredicateIndex + 2U]);
            return ctx.ChangeChildren(*node, std::move(children));
        }

        if (3U == node->ChildrenSize() && node->Child(1)->IsCallable("Bool") && node->Child(2)->IsCallable("Bool")) {
            const auto thenValue = FromString<bool>(node->Child(1)->Head().Content());
            const auto elseValue = FromString<bool>(node->Child(2)->Head().Content());

            if (thenValue != elseValue) {
                YQL_CLOG(DEBUG, Core) << node->Content() << " with literals in branches";
                return ctx.WrapByCallableIf(elseValue, "Not", node->HeadPtr());
            }
        }

        return node;
    };

    map["Chopper"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext&) {
        if (!IsDepended(node->Tail().Tail(), node->Tail().Head().Tail())) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " where handler isn't depended on group stream";
            return ctx.Builder(node->Pos())
                .Callable("OrderedFlatMap")
                    .Callable(0, "Condense1")
                        .Add(0, node->HeadPtr())
                        .Lambda(1)
                            .Param("item")
                            .Apply(*node->Child(1))
                                .With(0, "item")
                            .Seal()
                        .Seal()
                        .Lambda(2)
                            .Param("item")
                            .Param("key")
                            .Apply(*node->Child(2))
                                .With(0, "key")
                                .With(1, "item")
                            .Seal()
                        .Seal()
                        .Lambda(3)
                            .Param("item")
                            .Param("key")
                            .Arg("key")
                        .Seal()
                    .Seal()
                    .Lambda(1)
                        .Param("key")
                        .Apply(node->Tail())
                            .With(0, "key")
                            .With(1, node->HeadPtr())
                        .Seal()
                    .Seal()
                .Seal().Build();
        }

        return node;
    };

    map["IfPresent"] = std::bind(&OptimizeIfPresent<true>, _1, _2);

    map["TryMember"] = std::bind(&OptimizeTryMember, _1, _2);

    map["Optional"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        YQL_CLOG(DEBUG, Core) << node->Content();
        return ctx.NewCallable(node->Pos(), "Just", {node->TailPtr()});
    };

    map["List"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        if (node->ChildrenSize() > 1) {
            YQL_CLOG(DEBUG, Core) << "Non empty " << node->Content();
            TExprNode::TListType asListChildren(node->ChildrenList());
            asListChildren.erase(asListChildren.begin());
            return ctx.NewCallable(node->Pos(), "AsList", std::move(asListChildren));
        }

        return node;
    };

    map["OptionalReduce"] = std::bind(&RemoveOptionalReduceOverData, _1, _2);

    map["Fold"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        Y_UNUSED(ctx);
        if (node->Child(1)->GetTypeAnn()->GetKind() == ETypeAnnotationKind::Struct) {
            const auto count = node->Child(1)->GetTypeAnn()->Cast<TStructExprType>()->GetSize();
            if (count == 0) {
                YQL_CLOG(DEBUG, Core) << node->Content() << " with empty struct as state";
                return node->ChildPtr(1); // singleton
            }
        }
        else if (node->Child(1)->GetTypeAnn()->GetKind() == ETypeAnnotationKind::Tuple) {
            const auto count = node->Child(1)->GetTypeAnn()->Cast<TTupleExprType>()->GetSize();
            if (count == 0) {
                YQL_CLOG(DEBUG, Core) << node->Content() << " with empty tuple as state";
                return node->ChildPtr(1); // singleton
            }
        }

        return node;
    };

    map["Fold1"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        if (node->Child(1)->Tail().GetTypeAnn()->GetKind() == ETypeAnnotationKind::Struct) {
            const auto count = node->Child(1)->Tail().GetTypeAnn()->Cast<TStructExprType>()->GetSize();
            if (count == 0) {
                YQL_CLOG(DEBUG, Core) << node->Content() << " with empty struct as state";
                return ctx.Builder(node->Pos())
                    .Callable("OptionalIf")
                        .Callable(0, "HasItems")
                            .Add(0, node->HeadPtr())
                        .Seal()
                        .Callable(1, "AsStruct")
                        .Seal()
                    .Seal()
                    .Build();
            }
        }
        else if (node->Child(1)->Tail().GetTypeAnn()->GetKind() == ETypeAnnotationKind::Tuple) {
            const auto count = node->Child(1)->Tail().GetTypeAnn()->Cast<TTupleExprType>()->GetSize();
            if (count == 0) {
                YQL_CLOG(DEBUG, Core) << node->Content() << " with empty tuple as state";
                return ctx.Builder(node->Pos())
                    .Callable("OptionalIf")
                        .Callable(0, "HasItems")
                            .Add(0, node->HeadPtr())
                        .Seal()
                        .List(1)
                        .Seal()
                    .Seal()
                    .Build();
            }
        }

        return node;
    };

    map["GroupByKey"] = std::bind(&DropReorder, _1, _2);
    map["CombineByKey"] = std::bind(&DropReorder, _1, _2);

    map["ToList"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        if (node->Head().IsCallable("Nothing")) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
            return ctx.NewCallable(node->Pos(), "List", {ExpandType(node->Pos(), *node->GetTypeAnn(), ctx)});
        }

        if (node->Head().IsCallable("Just")) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
            return ctx.NewCallable(node->Head().Pos(), "AsList", {node->Head().HeadPtr()});
        }

        if (node->Head().IsCallable({"Head", "ToOptional"})) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
            auto ret = ctx.Builder(node->Pos())
                .Callable("Take")
                    .Add(0, node->Head().HeadPtr())
                    .Callable(1, "Uint64")
                        .Atom(0, "1", TNodeFlags::Default)
                    .Seal()
                .Seal()
                .Build();

            return ret;
        }

        if (node->Head().IsCallable("OptionalIf")) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
            return ctx.RenameNode(node->Head(), "ListIf");
        }

        return node;
    };

    map["ToStream"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        if (node->Head().IsCallable("Nothing")) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
            return ctx.NewCallable(node->Pos(), "EmptyIterator", {ExpandType(node->Pos(), *node->GetTypeAnn(), ctx)});
        }
        return node;
    };

    map["ToOptional"] = std::bind(OptimizeToOptional<true>, _1, _2);
    map["Head"] = std::bind(OptimizeToOptional<true>, _1, _2);
    map["Last"] = std::bind(OptimizeToOptional<false>, _1, _2);

    map["Not"] = std::bind(&SimplifyLogicalNot, _1, _2);
    map["And"] = std::bind(&SimplifyLogical<true>, _1, _2);
    map["Or"] = std::bind(&SimplifyLogical<false>, _1, _2);
    map["Xor"] = std::bind(&SimplifyLogicalXor, _1, _2);

    map["=="] = std::bind(&OptimizeEquality<true>, _1, _2);
    map["!="] = std::bind(&OptimizeEquality<false>, _1, _2);

    map["IsNotDistinctFrom"] = std::bind(&OptimizeDistinctFrom<true>, _1, _2);
    map["IsDistinctFrom"] = std::bind(&OptimizeDistinctFrom<false>, _1, _2);

    map["StartsWith"] = std::bind(&OptimizeEquality<true>, _1, _2);
    map["EndsWith"] = std::bind(&OptimizeEquality<true>, _1, _2);

    map["<"] = map["<="] = map[">"] = map[">="] = std::bind(&OptimizeCompare, _1, _2);;

    map["Sort"] = std::bind(&OptimizeReorder<false, true>, _1, _2);
    map["AssumeSorted"] = std::bind(&OptimizeReorder<false, false>, _1, _2);

    map["Top"] = std::bind(&OptimizeReorder<true, false>, _1, _2);
    map["TopSort"] = std::bind(&OptimizeReorder<true, true>, _1, _2);

    map["Minus"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        if (node->Head().IsCallable("Minus")) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
            return node->Head().HeadPtr();
        }

        if (TCoIntegralCtor::Match(&node->Head())) {
            YQL_CLOG(DEBUG, Core) << "Constant fold " << node->Content() << " over " << node->Head().Content() << " '" << node->Head().Head().Content();
            ui64 extracted;
            bool hasSign, isSigned;
            ExtractIntegralValue(node->Head(), true, hasSign, isSigned, extracted);
            const auto atomValue = GetIntegralAtomValue(extracted, hasSign && isSigned);
            return ctx.ChangeChild(node->Head(), 0U, ctx.NewAtom(node->Pos(), atomValue, TNodeFlags::Default));
        }

        return node;
    };

    map["Plus"] = [](const TExprNode::TPtr& node, TExprContext& /*ctx*/, TOptimizeContext& /*optCtx*/) {
        YQL_CLOG(DEBUG, Core) << node->Content();
        return node->HeadPtr();
    };

    map["CastStruct"] = std::bind(&ExpandCastStruct, _1, _2);

    map["Append"] = std::bind(&OptimizeInsert<true>, _1, _2, _3);
    map["Insert"] = std::bind(&OptimizeInsert<true>, _1, _2, _3);
    map["Prepend"] = std::bind(&OptimizeInsert<false>, _1, _2, _3);

    map["Extract"] = std::bind(&ExpandExtract<false>, _1, _2);
    map["OrderedExtract"] = std::bind(&ExpandExtract<true>, _1, _2);

    map["UnionAll"] = std::bind(&ExpandUnionAll<false>, _1, _2, _3);
    map["UnionMerge"] = std::bind(&ExpandUnionAll<true>, _1, _2, _3);

    map["Aggregate"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        TCoAggregate self(node);
        if (self.Keys().Size() == 0 && !HasPayload(self)) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " with empty fields";
            return ctx.NewCallable(node->Pos(), "AsList", {ctx.NewCallable(node->Pos(), "AsStruct", {})});
        }

        if (auto maybeAggregate = self.Input().Maybe<TCoAggregate>()) {
            auto child = maybeAggregate.Cast();
            if (!HasPayload(self) && !HasPayload(child) && self.Keys().Size() == child.Keys().Size()) {
                YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Content() << " without payload with same keys";
                return self.Input().Ptr();
            }
        }

        return DropReorder(node, ctx);
    };

    map["Min"] = std::bind(&OptimizeMinMax<true>, _1, _2);
    map["Max"] = std::bind(&OptimizeMinMax<false>, _1, _2);

    map["Unwrap"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        if (const auto& input = node->Head(); input.IsCallable("Just")) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << input.Content();
            return node->Head().HeadPtr();
        } else if (IsTransparentIfPresent(input)) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over transparent " << input.Content();
            return ctx.Builder(node->Pos())
                .ApplyPartial(input.Child(1U)->HeadPtr(), input.Child(1U)->Tail().HeadPtr())
                    .With(0U, ctx.ChangeChild(*node, 0U, input.HeadPtr()))
                .Seal().Build();
        }

        return node;
    };

    map["Reverse"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        if (node->Head().IsCallable("Reverse")) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
            return node->Head().HeadPtr();
        }

        if (node->Head().IsCallable({"Map", "FlatMap", "Filter", "Extend"})) {
            YQL_CLOG(DEBUG, Core) << "Drop " << node->Content() << " over unordered " << node->Head().Content();
            return node->HeadPtr();
        }

        if (node->Head().IsCallable("List") || node->Head().IsCallable("AsList")) {
            ui32 count = node->Head().ChildrenSize();
            if (node->Head().IsCallable("List")) {
                --count;
            }

            if (count <= 1) {
                YQL_CLOG(DEBUG, Core) << node->Content() << " over 0/1 literals";
                return node->HeadPtr();
            }
        }

        if (node->Head().IsCallable("AsList") && node->Head().ChildrenSize() > 1U) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
            auto children = node->Head().ChildrenList();
            std::reverse(children.begin(), children.end());
            return ctx.ChangeChildren(node->Head(), std::move(children));
        }

        return node;
    };

    map["EquiJoin"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) {
        auto ret = HandleEmptyListInJoin(node, ctx, *optCtx.Types);
        if (ret != node) {
            YQL_CLOG(DEBUG, Core) << "HandleEmptyListInJoin";
            return ret;
        }

        ret = HandleUniqueListInJoin(node, ctx, *optCtx.Types);
        if (ret != node) {
            YQL_CLOG(DEBUG, Core) << "HandleUniqueListInJoin";
            return ret;
        }

        ui32 inputsCount = node->ChildrenSize() - 2;
        for (ui32 i = 0; i < inputsCount; ++i) {
            if (IsListReorder(node->Child(i)->Head())) {
                YQL_CLOG(DEBUG, Core) << node->Content() << " with " << node->Child(i)->Content();
                return ctx.ChangeChild(*node, i, ctx.ChangeChild(*node->Child(i), 0, node->Child(i)->Head().HeadPtr()));
            }
        }

        ret = ExpandFlattenEquiJoin(node, ctx);
        if (ret != node) {
            YQL_CLOG(DEBUG, Core) << "ExpandFlattenEquiJoin";
            return ret;
        }

        ret = RemoveDeadPayloadColumns(node, ctx);
        if (ret != node) {
            YQL_CLOG(DEBUG, Core) << "RemoveDeadPayloadColumns in EquiJoin";
            return ret;
        }

        ret = PullAssumeColumnOrderOverEquiJoin(node, ctx, optCtx);
        if (ret != node) {
            YQL_CLOG(DEBUG, Core) << "Pull AssumeColumnOrder over EquiJoin";
            return ret;
        }

        return node;
    };

    map["Join"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        if (IsListReorder(node->Head())) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
            return ctx.ChangeChild(*node, 0, node->Head().HeadPtr());
        }

        if (IsListReorder(node->Tail())) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Tail().Content();
            return ctx.ChangeChild(*node, 1, node->Tail().HeadPtr());
        }

        return node;
    };

    map["AggrCountInit"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        if (node->Head().GetTypeAnn()->GetKind() != ETypeAnnotationKind::Optional || node->Head().IsCallable("Just")) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " - 1";
            return ctx.NewCallable(node->Pos(), "Uint64", { ctx.NewAtom(node->Pos(), "1", TNodeFlags::Default) });
        }

        if (node->Head().IsCallable("Nothing")) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " - 0";
            return ctx.NewCallable(node->Pos(), "Uint64", { ctx.NewAtom(node->Pos(), "0", TNodeFlags::Default) });
        }

        return node;
    };

    map["AggrCountUpdate"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        if (node->Head().GetTypeAnn()->GetKind() != ETypeAnnotationKind::Optional || node->Head().IsCallable("Just")) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " - Inc";
            return ctx.NewCallable(node->Pos(), "Inc", { node->TailPtr() });
        }

        if (node->Head().IsCallable("Nothing")) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " - None";
            return node->TailPtr();
        }

        return node;
    };

    map["Guess"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        if (node->Head().IsCallable("Nothing")) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
            return ctx.ChangeChild(node->Head(), 0U, ExpandType(node->Pos(), *node->GetTypeAnn(), ctx));
        }

        if (node->Head().IsCallable("Just")) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
            return ctx.ChangeChild(*node, 0U, node->Head().HeadPtr());
        }

        if (node->Head().IsCallable("Variant")) {
            if (node->Tail().Content() == node->Head().Child(1)->Content()) {
                YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content() << " - same index";
                return ctx.NewCallable(node->Pos(), "Just", { node->Head().HeadPtr() });
            } else {
                YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content() << " - different index";
                return ctx.NewCallable(node->Pos(), "Nothing", { ExpandType(node->Pos(), *node->GetTypeAnn(), ctx) });
            }
        }

        return node;
    };

    map["Way"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        if (node->Head().IsCallable("Nothing")) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
            return ctx.NewCallable(node->Pos(), "Nothing", { ExpandType(node->Pos(), *node->GetTypeAnn(), ctx) });
        }

        if (node->Head().IsCallable("Just")) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
            return ctx.NewCallable(node->Pos(), "Just", { ctx.NewCallable(node->Pos(), "Way", { node->Head().HeadPtr() }) });
        }

        if (node->Head().IsCallable("Variant")) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
            if (node->Head().GetTypeAnn()->Cast<TVariantExprType>()->GetUnderlyingType()->GetKind() == ETypeAnnotationKind::Tuple) {
                return ctx.NewCallable(node->Pos(), "Uint32", { node->Head().ChildPtr(1) });
            } else {
                return ctx.NewCallable(node->Pos(), "Utf8", { node->Head().ChildPtr(1) });
            }
        }

        return node;
    };

    map["Visit"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        if (node->ChildrenSize() == 2) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " - only default value";
            return node->TailPtr();
        }

        if (node->ChildrenSize() == 4) {
            // one handler and default value
            auto lambda = node->Child(2);
            auto defaultValue = node->Child(3);
            if (defaultValue->IsCallable("Nothing") && lambda->Tail().IsCallable("Just") &&
                &lambda->Tail().Tail() == &lambda->Head().Head()) {
                YQL_CLOG(DEBUG, Core) << node->Content() << " - convert to Guess";
                return ctx.NewCallable(node->Pos(), "Guess", { node->HeadPtr(), node->ChildPtr(1) });
            }

            auto varType = node->Head().GetTypeAnn()->Cast<TVariantExprType>();
            bool removeDefaultValue;
            if (varType->GetUnderlyingType()->GetKind() == ETypeAnnotationKind::Tuple) {
                removeDefaultValue = (varType->GetUnderlyingType()->Cast<TTupleExprType>()->GetSize() == 1);
            } else {
                removeDefaultValue = (varType->GetUnderlyingType()->Cast<TStructExprType>()->GetSize() == 1);
            }

            if (removeDefaultValue) {
                YQL_CLOG(DEBUG, Core) << node->Content() << " - remove default value";
                return ctx.NewCallable(node->Pos(), "Visit", { node->HeadPtr(), node->ChildPtr(1), node->ChildPtr(2) });
            }
        }

        if (node->Head().IsCallable("Variant")) {
            const auto& var = node->Head();
            for (ui32 index = 1; index < node->ChildrenSize(); index += 2) {
                auto child = node->ChildPtr(index);
                if (!child->IsAtom()) {
                    YQL_CLOG(DEBUG, Core) << node->Content() << " - substitute the default value";
                    return child;
                }

                if (child->Content() == var.Child(1)->Content()) {
                    YQL_CLOG(DEBUG, Core) << node->Content() << " - substitute the alternative";
                    // one handler and no default value
                    auto lambda = node->Child(index + 1);
                    return ctx.Builder(node->Pos())
                        .Apply(lambda)
                            .With(0, var.HeadPtr())
                        .Seal()
                        .Build();
                }
            }
        }

        if (node->ChildrenSize() % 2 == 1) { // No default value
            bool allJust = true;
            TNodeSet uniqLambdas;
            for (ui32 index = 1; index < node->ChildrenSize(); index += 2) {
                uniqLambdas.insert(node->Child(index + 1));
                if (!TCoJust::Match(node->Child(index + 1)->Child(1))) {
                    allJust = false;
                }
            }

            if (uniqLambdas.size() == 1 && node->ChildrenSize() > 3) {
                YQL_CLOG(DEBUG, Core) << node->Content() << " - all equal lambdas";
                return ctx.Builder(node->Pos())
                    .Apply(node->ChildPtr(2))
                        .With(0)
                            .Callable("VariantItem")
                                .Add(0, node->HeadPtr())
                            .Seal()
                        .Done()
                    .Seal()
                    .Build();
            }

            if (allJust) {
                YQL_CLOG(DEBUG, Core) << node->Content() << " - extract Just";
                return ctx.Builder(node->Pos())
                    .Callable("Just")
                        .Callable(0, "Visit")
                            .Add(0, node->HeadPtr())
                            .Do([&](TExprNodeBuilder& parent) -> TExprNodeBuilder& {
                                for (ui32 i = 1; i < node->ChildrenSize(); i += 2) {
                                    parent.Add(i, node->ChildPtr(i));
                                    auto visitLambda = node->Child(i + 1);
                                    parent.Lambda(i + 1, visitLambda->Pos())
                                        .Param("item")
                                        .ApplyPartial(visitLambda->HeadPtr(), visitLambda->Tail().HeadPtr())
                                            .With(0, "item")
                                        .Seal()
                                        .Seal();
                                }
                                return parent;
                            })
                        .Seal()
                    .Seal()
                    .Build();
            }
        }

        return node;
    };

    map[LeftName] = std::bind(&OptimizeDirection<false>, _1);
    map[RightName] = std::bind(&OptimizeDirection<true>, _1);

    map["Apply"] = [](const TExprNode::TPtr& node, TExprContext& /*ctx*/, TOptimizeContext& /*optCtx*/) {
        auto ret = FoldYsonParseAfterSerialize(node);
        if (ret != node) {
            return ret;
        }

        ret = FoldYson2ParseAfterSerialize(node);
        if (ret != node) {
            return ret;
        }

        ret = FoldYsonSeralizeAfterParse(node);
        if (ret != node) {
            return ret;
        }

        ret = FoldYson2SeralizeAfterParse(node);
        if (ret != node) {
            return ret;
        }

        ret = FoldJsonParseAfterSerialize(node); 
        if (ret != node) { 
            return ret; 
        } 
 
        ret = FoldJsonSeralizeAfterParse(node); 
        if (ret != node) { 
            return ret; 
        } 
 
        return node;
    };

    map["Switch"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        TExprNode::TPtr flatMap;
        for (auto i = 3U; !flatMap && i < node->ChildrenSize(); ++++i) {
            flatMap = FindNode(node->Child(i)->TailPtr(),
                [handler = node->Child(i)](const TExprNode::TPtr& child) {
                    return child->IsCallable({"FlatMap", "OrderedFlatMap"}) && child->Head().IsCallable() && child->Head().IsComplete() && child->Tail().GetDependencyScope()->first == handler
                        && (ETypeAnnotationKind::Flow == child->Head().GetTypeAnn()->GetKind() || ETypeAnnotationKind::Stream == child->Head().GetTypeAnn()->GetKind());
                }
            );
        }

        if (flatMap) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " bring out " << flatMap->Content() << " by independent " << flatMap->Head().Content() << " from handler.";
            auto children = node->ChildrenList();
            const auto arg = ctx.NewArgument(flatMap->Tail().Head().Head().Pos(), "outsider");
            TNodeOnNodeOwnedMap replaces((children.size() >> 1U) - 1U);
            for (auto i = 3U; i < children.size(); ++++i) {
                const auto ins = replaces.emplace(children[i].Get(), TExprNode::TPtr());
                if (ins.second) {
                    ins.first->second = ctx.DeepCopyLambda(*ins.first->first, ctx.ReplaceNode(ins.first->first->TailPtr(), *flatMap, ctx.ReplaceNode(flatMap->Tail().TailPtr(), flatMap->Tail().Head().Head(), arg)));
                }
                children[i] = ins.first->second;
            }
            return ctx.ChangeChildren(*flatMap, {CloneCompleteFlow(flatMap->HeadPtr(), ctx), ctx.NewLambda(flatMap->Tail().Pos(), ctx.NewArguments(flatMap->Tail().Head().Pos(), {arg}), ctx.ChangeChildren(*node, std::move(children)))});
        }

        const auto inputItemType = GetSeqItemType(node->Head().GetTypeAnn());
        const bool singleInput = inputItemType->GetKind() != ETypeAnnotationKind::Variant;

        TDynBitMap usedIndicies;
        TExprNode::TListType lambdas;
        TExprNode::TListType indicies;
        TExprNode::TListType castStructs;
        ETypeAnnotationKind targetType = singleInput ? ETypeAnnotationKind::List : ETypeAnnotationKind::Optional;
        const bool singleHandler = node->ChildrenSize() == 4;
        bool ordered = false;

        if (singleInput && singleHandler && node->Child(2)->ChildrenSize() == 1) { // Exactly one index
            YQL_CLOG(DEBUG, Core) << node->Content() << " with single input and single handler";
            return ctx.Builder(node->Pos())
                .Apply(node->ChildPtr(3)) // handler lambda
                    .With(0, node->HeadPtr()) // Switch input
                .Seal()
                .Build();
        }

        for (ui32 i = 2; i < node->ChildrenSize(); i += 2) {
            if (node->Child(i)->ChildrenSize() != 1) {
                return node;
            }
            if (!singleInput) {
                ui32 index = FromString<ui32>(node->Child(i)->Head().Content());
                if (usedIndicies.Test(index)) {
                    return node;
                }
                usedIndicies.Set(index);
                indicies.push_back(node->Child(i)->HeadPtr());
            }

            auto lambda = node->Child(i + 1);
            if (&lambda->Head().Head() == &lambda->Tail()) {
                // Trivial lambda
                ordered = ordered || lambda->GetConstraint<TSortedConstraintNode>();
                lambdas.emplace_back();
                castStructs.emplace_back();
            }
            else if (TCoFlatMapBase::Match(lambda->Child(1))) {
                ordered = ordered || TCoOrderedFlatMap::Match(lambda->Child(1)) || lambda->GetConstraint<TSortedConstraintNode>();
                auto flatMapInput = lambda->Child(1)->Child(0);
                const TTypeAnnotationNode* castType = nullptr;
                if (TCoExtractMembers::Match(flatMapInput)) {
                    castType = GetSeqItemType(flatMapInput->GetTypeAnn());
                    flatMapInput = flatMapInput->Child(0);
                }

                if (flatMapInput != lambda->Head().Child(0)) { // FlatMap input == Switch lambda arg
                    return node;
                }

                auto flatMapLambda = lambda->Child(1)->ChildPtr(1);
                switch (flatMapLambda->GetTypeAnn()->GetKind()) {
                case ETypeAnnotationKind::Optional:
                    if (!singleHandler && flatMapLambda->GetTypeAnn()->Cast<TOptionalExprType>()->GetItemType()->GetKind() == ETypeAnnotationKind::Variant) {
                        return node;
                    }
                    break;
                case ETypeAnnotationKind::List:
                    if (!singleHandler && flatMapLambda->GetTypeAnn()->Cast<TListExprType>()->GetItemType()->GetKind() == ETypeAnnotationKind::Variant) {
                        return node;
                    }
                    if (targetType != ETypeAnnotationKind::Stream) {
                        targetType = ETypeAnnotationKind::List;
                    }
                    break;
                case ETypeAnnotationKind::Stream:
                    if (!singleHandler && flatMapLambda->GetTypeAnn()->Cast<TStreamExprType>()->GetItemType()->GetKind() == ETypeAnnotationKind::Variant) {
                        return node;
                    }
                    targetType = ETypeAnnotationKind::Stream;
                    break;
                case ETypeAnnotationKind::Flow:
                    if (!singleHandler && flatMapLambda->GetTypeAnn()->Cast<TFlowExprType>()->GetItemType()->GetKind() == ETypeAnnotationKind::Variant) {
                        return node;
                    }
                    targetType = ETypeAnnotationKind::Flow;
                    break;
                default:
                    YQL_ENSURE(false, "Unsupported FlatMap lambda return type: " << flatMapLambda->GetTypeAnn()->GetKind());
                }
                lambdas.push_back(std::move(flatMapLambda));
                castStructs.push_back(castType ? ExpandType(flatMapInput->Pos(), *castType, ctx) : TExprNode::TPtr());
            }
            else {
                return node;
            }
        }

        const auto flatMapName = ordered ? TCoOrderedFlatMap::CallableName() : TCoFlatMap::CallableName();
        const auto mapName = ordered ? TCoOrderedMap::CallableName() : TCoMap::CallableName();
        if (indicies.size() == 1) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " with single trivial or FlatMap lambda";
            if (lambdas.front()) {
                return ctx.Builder(node->Pos())
                    .Callable(flatMapName)
                        .Callable(0, flatMapName)
                            .Add(0, node->HeadPtr())
                            .Lambda(1)
                                .Param("item")
                                .Callable("Guess")
                                    .Arg(0, "item")
                                    .Add(1, indicies.front())
                                .Seal()
                            .Seal()
                        .Seal()
                        .Lambda(1)
                            .Param("varItem")
                            .Apply(lambdas.front())
                                .With(0)
                                    .Do([&](TExprNodeBuilder& builder) -> TExprNodeBuilder& {
                                        if (castStructs.front()) {
                                            builder.Callable("CastStruct")
                                                .Arg(0, "varItem")
                                                .Add(1, castStructs.front())
                                                .Seal();
                                        } else {
                                            builder.Arg("varItem");
                                        }
                                        return builder;
                                    })
                                .Done()
                            .Seal()
                        .Seal()
                    .Seal()
                    .Build();
            }
            else {
                return ctx.Builder(node->Pos())
                    .Callable(flatMapName)
                        .Add(0, node->HeadPtr())
                        .Lambda(1)
                            .Param("item")
                            .Callable("Guess")
                                .Arg(0, "item")
                                .Add(1, indicies.front())
                            .Seal()
                        .Seal()
                    .Seal()
                    .Build();
            }
        }

        const auto outVarType = ExpandType(node->Pos(), *GetSeqItemType(node->GetTypeAnn()), ctx);

        TExprNode::TListType updatedLambdas;
        for (size_t i = 0; i < lambdas.size(); ++i) {
            auto arg = ctx.NewArgument(node->Pos(), "varItem");
            TExprNode::TPtr body;
            if (lambdas[i]) {
                body = ctx.Builder(node->Pos())
                    .Callable(mapName)
                        .Apply(0, lambdas[i])
                            .With(0)
                                .Do([&](TExprNodeBuilder& builder) -> TExprNodeBuilder& {
                                    if (castStructs[i]) {
                                        builder.Callable("CastStruct")
                                            .Add(0, arg)
                                            .Add(1, castStructs[i])
                                            .Seal();
                                    } else {
                                        builder.Arg(arg);
                                    }
                                    return builder;
                                })
                            .Done()
                        .Seal()
                        .Lambda(1)
                            .Param("mapItem")
                            .Callable("Variant")
                                .Arg(0, "mapItem")
                                .Atom(1, ToString(i), TNodeFlags::Default)
                                .Add(2, outVarType)
                            .Seal()
                        .Seal()
                    .Seal()
                    .Build();
                if (lambdas[i]->GetTypeAnn()->GetKind() != targetType) {
                    switch (targetType) {
                        case ETypeAnnotationKind::Flow:
                            body = ctx.NewCallable(node->Pos(), "ToFlow", {std::move(body)});
                            break;
                        case ETypeAnnotationKind::Stream:
                            body = ctx.NewCallable(node->Pos(), "ToStream", {std::move(body)});
                            break;
                        case ETypeAnnotationKind::List:
                            body = ctx.NewCallable(node->Pos(), "ToList", {std::move(body)});
                            break;
                        default:
                            break;
                    }
                }
            }
            else {
                body = ctx.Builder(node->Pos())
                    .Callable("Variant")
                        .Add(0, arg)
                        .Atom(1, ToString(i), TNodeFlags::Default)
                        .Add(2, outVarType)
                    .Seal()
                    .Build();
                if (ETypeAnnotationKind::List == targetType) {
                    body = ctx.NewCallable(node->Pos(), "AsList", {std::move(body)});
                }
                else {
                    body = ctx.NewCallable(node->Pos(), "Just", {std::move(body)});
                    if (ETypeAnnotationKind::Flow == targetType) {
                        body = ctx.NewCallable(node->Pos(), "ToFlow", {std::move(body)});
                    }
                    else if (ETypeAnnotationKind::Stream == targetType) {
                        body = ctx.NewCallable(node->Pos(), "ToStream", {std::move(body)});
                    }
                }
            }
            updatedLambdas.push_back(ctx.NewLambda(node->Pos(), ctx.NewArguments(node->Pos(), {std::move(arg)}), std::move(body)));
        }

        if (singleInput) {
            YQL_CLOG(DEBUG, Core) << "Replicating " << node->Content() << " with trivial or FlatMap lambdas";
            return ctx.Builder(node->Pos())
                .Callable(flatMapName)
                    .Add(0, node->HeadPtr())
                    .Lambda(1)
                        .Param("item")
                        .Callable(ordered ? TCoOrderedExtend::CallableName() : TCoExtend::CallableName())
                            .Do([&](TExprNodeBuilder& builder) -> TExprNodeBuilder& {
                                for (size_t i = 0; i < updatedLambdas.size(); ++i) {
                                    builder.Apply(i, *updatedLambdas[i])
                                        .With(0, "item")
                                    .Seal();
                                }
                                return builder;
                            })
                        .Seal()
                    .Seal()
                .Seal()
                .Build();
        }

        const auto inputVarTupleType = inputItemType->Cast<TVariantExprType>()->GetUnderlyingType()->Cast<TTupleExprType>();

        YQL_CLOG(DEBUG, Core) << node->Content() << " with trivial or FlatMap lambdas";
        return ctx.Builder(node->Pos())
            .Callable(flatMapName)
                .Add(0, node->HeadPtr())
                .Lambda(1)
                    .Param("item")
                    .Callable("Visit")
                        .Arg(0, "item")
                        .Do([&](TExprNodeBuilder& builder) -> TExprNodeBuilder& {
                            for (size_t i = 0; i < indicies.size(); ++i) {
                                builder.Add(i * 2 + 1, indicies[i]);
                                builder.Add(i * 2 + 2, updatedLambdas[i]);
                            }
                            if (indicies.size() < inputVarTupleType->GetSize()) {
                                builder.Callable(indicies.size() * 2 + 1, GetEmptyCollectionName(targetType))
                                    .Add(0, ExpandType(node->Pos(), *MakeSequenceType(targetType, *GetSeqItemType(node->GetTypeAnn()), ctx), ctx))
                                .Seal();
                            }
                            return builder;
                        })
                    .Seal()
                .Seal()
            .Seal()
            .Build();
    };

    map["VariantItem"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        if (TCoJust::Match(&node->Head())) {
            YQL_CLOG(DEBUG, Core) << "Move " << node->Content() << " over " << node->Head().Content();
            return ctx.SwapWithHead(*node);
        }
        if (TCoOptionalIf::Match(&node->Head())) {
            YQL_CLOG(DEBUG, Core) << "Move " << node->Content() << " over " << node->Head().Content();
            return ctx.ChangeChild(node->Head(), 1U, ctx.ChangeChild(*node, 0U, node->Head().TailPtr()));
        }
        if (TCoVariant::Match(&node->Head())) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
            return node->Head().HeadPtr();
        }
        if (TCoNothing::Match(&node->Head())) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
            return ctx.Builder(node->Pos())
                .Callable("Nothing")
                    .Add(0, ExpandType(node->Pos(), *node->GetTypeAnn(), ctx))
                .Seal()
                .Build();
        }
        return node;
    };

    map["Untag"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        Y_UNUSED(ctx);
        if (node->Head().IsCallable("AsTagged")) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
            return node->Head().HeadPtr();
        }

        return node;
    };

    map["SqueezeToDict"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) {
        if (const auto& inputToCheck = SkipCallables(node->Head(), SkippableCallables); IsEmptyContainer(inputToCheck) || IsEmpty(inputToCheck, *optCtx.Types)) {
            YQL_CLOG(DEBUG, Core) << "Empty " << node->Content();
            return ctx.Builder(node->Pos())
                .Callable(ETypeAnnotationKind::Flow == node->GetTypeAnn()->GetKind() ? "ToFlow" : "ToStream")
                    .Callable(0, "Just")
                        .Callable(0, "Dict")
                            .Add(0, ExpandType(node->Pos(), *GetSeqItemType(node->GetTypeAnn()), ctx))
                        .Seal()
                    .Seal()
                .Seal().Build();
        }

        return node;
    };

    map["ToDict"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) {
        if (const auto& inputToCheck = SkipCallables(node->Head(), SkippableCallables); IsEmptyContainer(inputToCheck) || IsEmpty(inputToCheck, *optCtx.Types)) {
            YQL_CLOG(DEBUG, Core) << "Empty " << node->Content();
            return ctx.NewCallable(inputToCheck.Pos(), "Dict", {ExpandType(node->Pos(), *node->GetTypeAnn(), ctx)});
        }

        if (node->Head().IsCallable("AsList") && node->Child(2)->Child(1)->IsCallable("Void")) {
            TMaybe<bool> isMany;
            TMaybe<bool> isHashed;
            TMaybe<ui64> itemsCount;
            bool isCompact;
            auto settingsError = ParseToDictSettings(*node, ctx, isMany, isHashed, itemsCount, isCompact);
            YQL_ENSURE(!settingsError);

            if (!*isMany && *isHashed) {
                YQL_CLOG(DEBUG, Core) << "ToDict without payload over list literal";
                return ctx.Builder(node->Pos())
                    .Callable("DictFromKeys")
                        .Add(0, ExpandType(node->Pos(), *node->GetTypeAnn()->Cast<TDictExprType>()->GetKeyType(), ctx))
                        .List(1)
                            .Do([&](TExprNodeBuilder& builder) -> TExprNodeBuilder& {
                                for (ui32 i = 0; i < node->Head().ChildrenSize(); ++i) {
                                    builder.Apply(i, node->ChildPtr(1))
                                        .With(0, node->Head().ChildPtr(i))
                                    .Seal();
                                }
                                return builder;
                            })
                        .Seal()
                    .Seal()
                    .Build();
            }
        }

        if (node->Head().IsCallable("DictItems")) {
            auto inner = node->Head().ChildPtr(0);
            if (inner->IsCallable("ToDict")) {
                auto keyLambda = node->Child(1);
                auto payloadLambda = node->Child(2);
                auto settings = node->Child(3);
                auto innerSettings = inner->Child(3);
                bool sameType = AnyOf(settings->Children(), [](const auto& x) { return x->Content() == "Hashed"; }) ==
                    AnyOf(innerSettings->Children(), [](const auto& x) { return x->Content() == "Hashed"; });

                if (sameType
                    && keyLambda->Child(1)->IsCallable("Nth") && keyLambda->Child(1)->Child(1)->Content() == "0"
                    && keyLambda->Child(1)->Child(0) == keyLambda->Child(0)->Child(0)
                    && payloadLambda->Child(1)->IsCallable("Nth") && payloadLambda->Child(1)->Child(1)->Content() == "1"
                    && payloadLambda->Child(1)->Child(0) == payloadLambda->Child(0)->Child(0)
                    && !AnyOf(settings->Children(), [](const auto& x) { return x->Content() == "Many"; })) {
                    YQL_CLOG(DEBUG, Core) << "ToDict over DictItems";
                    return inner;
                }
            }
        }

        return node;
    };

    map["HasNull"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        YQL_CLOG(DEBUG, Core) << node->Content();

        auto value = node->HeadPtr();
        auto valueType = value->GetTypeAnn();

        if (!valueType->HasOptionalOrNull()) {
            return MakeBool<false>(node->Pos(), ctx);
        }

        switch (valueType->GetKind()) {
            case ETypeAnnotationKind::Null:
                return MakeBool<true>(node->Pos(), ctx);
            case ETypeAnnotationKind::Optional:
                return ctx.Builder(node->Pos())
                    .Callable("IfPresent")
                        .Add(0, value)
                        .Lambda(1)
                            .Param("item")
                            .Callable("HasNull")
                                .Arg(0, "item")
                            .Seal()
                        .Seal()
                        .Add(2, MakeBool<true>(node->Pos(), ctx))
                    .Seal()
                    .Build();
            case ETypeAnnotationKind::Tagged:
                return ctx.Builder(node->Pos())
                    .Callable("HasNull")
                        .Callable(0, "Untag")
                            .Add(0, value)
                            .Atom(1, valueType->Cast<TTaggedExprType>()->GetTag())
                        .Seal()
                    .Seal()
                    .Build();
            case ETypeAnnotationKind::Dict:
                return ctx.Builder(node->Pos())
                    .Callable("HasNull")
                        .Callable(0, "DictItems")
                            .Add(0, value)
                        .Seal()
                    .Seal()
                    .Build();
            case ETypeAnnotationKind::List:
                return ctx.Builder(node->Pos())
                    .Callable("HasItems")
                        .Callable(0, "SkipWhile")
                            .Add(0, value)
                            .Lambda(1)
                                .Param("item")
                                .Callable("Not")
                                    .Callable(0, "HasNull")
                                        .Arg(0, "item")
                                    .Seal()
                                .Seal()
                            .Seal()
                        .Seal()
                    .Seal()
                    .Build();
            case ETypeAnnotationKind::Tuple:
                return HasNullOverTuple(node, ctx);
            case ETypeAnnotationKind::Struct:
                return HasNullOverStruct(node, ctx);
            case ETypeAnnotationKind::Variant:
                return HasNullOverVariant(node, ctx);
            default:
                YQL_ENSURE(false, "Value type " << *valueType << " is not supported!");
        }

        Y_UNREACHABLE();
    };

    map["Unordered"] = map["UnorderedSubquery"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        if (node->Head().IsCallable("AsList")) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
            return node->HeadPtr();
        }

        if (node->Head().IsCallable("AssumeSorted")) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
            return ctx.ChangeChild(*node, 0, node->Head().HeadPtr());
        }

        return node;
    };

    map["Demux"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        if (TCoExtendBase::Match(&node->Head())) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();

            TExprNode::TListType demuxChildren;
            std::transform(node->Head().Children().begin(), node->Head().Children().end(),
                std::back_inserter(demuxChildren),
                [&] (const TExprNode::TPtr& n) {
                    return Build<TCoDemux>(ctx, n->Pos()).Input(n).Done().Ptr();
                }
            );

            auto variantType = node->Head().GetTypeAnn()->Cast<TListExprType>()->GetItemType()->Cast<TVariantExprType>();
            if (variantType->GetUnderlyingType()->GetKind() == ETypeAnnotationKind::Tuple) {
                TExprNode::TListType resChildren;
                for (size_t i = 0; i < variantType->GetUnderlyingType()->Cast<TTupleExprType>()->GetSize(); ++i) {
                    const auto nthIndex = ctx.NewAtom(node->Pos(), ToString(i));
                    TExprNode::TListType extendChildren;
                    for (auto& demux: demuxChildren) {
                        extendChildren.push_back(Build<TCoNth>(ctx, demux->Pos())
                            .Tuple(demux)
                            .Index(nthIndex)
                            .Done().Ptr()
                        );
                    }
                    resChildren.push_back(ctx.NewCallable(node->Pos(), node->Head().Content(), std::move(extendChildren)));
                }
                return ctx.NewList(node->Pos(), std::move(resChildren));
            }
            else {
                TExprNode::TListType resChildren;
                for (auto structItem: variantType->GetUnderlyingType()->Cast<TStructExprType>()->GetItems()) {
                    const auto memberName = ctx.NewAtom(node->Pos(), structItem->GetName());
                    TExprNode::TListType extendChildren;
                    for (auto& demux: demuxChildren) {
                        extendChildren.push_back(Build<TCoMember>(ctx, demux->Pos())
                            .Struct(demux)
                            .Name(memberName)
                            .Done().Ptr()
                        );
                    }
                    auto extend = ctx.NewCallable(node->Pos(), node->Head().Content(), std::move(extendChildren));
                    resChildren.push_back(ctx.NewList(node->Pos(), {memberName, extend}));
                }
                return ctx.NewCallable(node->Pos(), TCoAsStruct::CallableName(), std::move(resChildren));
            }
        }

        if (TCoMux::Match(&node->Head())) {
            auto variantType = node->Head().GetTypeAnn()->Cast<TListExprType>()->GetItemType()->Cast<TVariantExprType>();
            if (variantType->GetUnderlyingType()->GetKind() == ETypeAnnotationKind::Tuple && node->Head().Head().IsList()) {
                YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
                return node->Head().HeadPtr();
            }
        }

        return node;
    };
 
    map["JsonValue"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) { 
        /* 
        Here we rewrite expression 
            JSON_VALUE( 
                <json>, <jsonPath> 
                [PASSING <variableExpr1> AS <variableName1>, ...] 
                [RETURNING <resultType>] 
                [(NULL | DEFAULT <onEmptyExpr>) ON EMPTY] 
                [(NULL | DEFAULT <onErrorExpr>) ON ERROR] 
            ) 
        Generated depends on the <resultType> specified in RETURNING section: 
            1. No RETURNING section 
                Default returning type of JsonValue is Utf8 and it must convert 
                result of JsonPath expression into Utf8 string. 
                Json2::SqlValueConvertToUtf8 is used 
            2. <resultType> is a numeric type (Int16, Uint16, Float, etc.) 
                Json2::SqlValueNumber is used with additional CAST to corresponding type 
            3. <resultType> is a date type (Date, Datetime, Timestamp) 
                Json2::SqlValueInt64 is used with additional CAST to corresponding type 
            4. <resultType> is Bool 
                Json2::SqlValueBool is used 
            5. <resultType> is String 
                Json2::SqlValueUtf8 is used with additional CAST to String 
            6. <resultType> is Utf8 
                Json2::SqlValueUtf8 is used 
        Returning type of all Json2::SqlValue* functions is Variant<Tuple<Uint8, String?>, <resultType>?>: 
            1. If variant holds first type, either error happened or the result is empty. 
                If first tuple element is 0, result is empty. 
                If first tuple element is 1, error happened. 
                Second tuple element contains message that can be displayed to the user. 
            2. If variant hold second type, execution was successful and it is a result. 
        We process result of Json2::SqlValue* function by using Visit callable with lambdas handling each type. 
        Note that in some cases we need to CAST result of Json2::SqlValue* and it can fail. So: 
            1. If the result of Json2::SqlValue* is NULL, we return Nothing(<resultType>) 
            2. Otherwise we check the result of SafeCast callable. If it is NULL, cast has failed and it is an error. 
                If it holds some value, we return it to the user. 
        If no CAST is needed, we just return the result of Json2::SqlValue*. 
        What is more, <onEmptyExpr> and <onErrorExpr> must be casted to <resultType> and this CAST can fail too. 
        ANSI SQL specification is unclear about what to do with this situation. If we failed to cast <onEmptyExpr> to 
        target type, we return <onErrorExpr>. If we failed to cast <onErrorExpr> to target type, we throw an exception. 
 
        I know all this sounds very clumsy and a lot of logic to handle in s-expressions. If you have a better idea 
        of a way to handle all this ***, please write to laplab@. 
        */ 
        TCoJsonValue jsonValue(node); 
 
        // <json expr> or Json2::Parse(<json expr>) 
        EDataSlot jsonDataSlot; 
        TExprNode::TPtr jsonExpr = GetJsonDocumentOrParseJson(jsonValue, ctx, jsonDataSlot); 
 
        // Json2::CompilePath(<jsonPath>) 
        TExprNode::TPtr compilePathExpr = BuildJsonCompilePath(jsonValue, ctx); 
 
        // Json2::SqlValue...(<parsedJson>, <compiledJsonPath>) 
        TExprNode::TPtr sqlValueExpr; 
        const auto returnTypeAnn = node->GetTypeAnn()->Cast<TOptionalExprType>(); 
        const auto unwrappedSlot = returnTypeAnn->GetItemType()->Cast<TDataExprType>()->GetSlot(); 
        bool needCast = false; 
        const auto jsonValuePos = jsonValue.Pos(); 
        { 
            TString sqlValueUdfName; 
            if (IsDataTypeNumeric(unwrappedSlot)) { 
                sqlValueUdfName = "SqlValueNumber"; 
                needCast = true; 
            } else if (IsDataTypeDate(unwrappedSlot)) { 
                sqlValueUdfName = "SqlValueInt64"; 
                needCast = true; 
            } else if (unwrappedSlot == EDataSlot::Utf8 || unwrappedSlot == EDataSlot::String) { 
                if (jsonValue.ReturningType()) { 
                    sqlValueUdfName = "SqlValueUtf8"; 
                } else { 
                    sqlValueUdfName = "SqlValueConvertToUtf8"; 
                } 
                needCast = unwrappedSlot == EDataSlot::String; 
            } else if (unwrappedSlot == EDataSlot::Bool) { 
                sqlValueUdfName = "SqlValueBool"; 
            } else { 
                YQL_ENSURE(false, "Unsupported type"); 
            } 
 
            const TTypeAnnotationNode* inputType = nullptr; 
            if (jsonDataSlot == EDataSlot::JsonDocument) { 
                inputType = ctx.MakeType<TOptionalExprType>(ctx.MakeType<TDataExprType>(EDataSlot::JsonDocument)); 
                sqlValueUdfName = "JsonDocument" + sqlValueUdfName; 
            } else { 
                inputType = ctx.MakeType<TOptionalExprType>(ctx.MakeType<TResourceExprType>("JsonNode")); 
            } 
            sqlValueUdfName = "Json2." + sqlValueUdfName; 
 
            TTypeAnnotationNode::TListType arguments = { 
                inputType, 
                ctx.MakeType<TResourceExprType>("JsonPath") 
            }; 
 
            auto udfArgumentsType = ctx.MakeType<TTupleExprType>(TTypeAnnotationNode::TListType{ 
                ctx.MakeType<TTupleExprType>(arguments), 
                ctx.MakeType<TStructExprType>(TVector<const TItemExprType*>{}), 
                ctx.MakeType<TTupleExprType>(TTypeAnnotationNode::TListType{}) 
            }); 
 
            auto sqlValue = Build<TCoUdf>(ctx, jsonValuePos) 
                .MethodName() 
                    .Build(sqlValueUdfName) 
                .RunConfigValue<TCoVoid>() 
                    .Build() 
                .UserType(ExpandType(jsonValuePos, *udfArgumentsType, ctx)) 
                .Done().Ptr(); 
 
            sqlValueExpr = Build<TCoApply>(ctx, jsonValuePos) 
                .Callable(sqlValue) 
                .FreeArgs() 
                    .Add(jsonExpr) 
                    .Add(compilePathExpr) 
                    .Add(jsonValue.Variables()) 
                    .Build() 
                .Done().Ptr(); 
        } 
 
        auto makeCastOrValue = [&](TPositionHandle pos, const TExprNode::TPtr& source, const TExprNode::TPtr& onCastFail) { 
            /* 
            if Exists($source) 
            then 
                return IfPresent( 
                    CAST($source as <resultType>), 
                    ($x) -> { return Just($x); }, 
                    $onCastFail 
                ) 
            else 
                return Nothing(<resultType>) 
            */ 
            TExprNode::TPtr returnTypeNode = ExpandType(pos, *returnTypeAnn, ctx); 
            return Build<TCoIf>(ctx, pos) 
                .Predicate<TCoExists>() 
                    .Optional(source) 
                    .Build() 
                .ThenValue<TCoIfPresent>() 
                    .Optional<TCoSafeCast>() 
                        .Value(source) 
                        .Type(returnTypeNode) 
                            .Build() 
                    .PresentHandler<TCoLambda>() 
                        .Args({"unwrappedValue"}) 
                        .Body<TCoJust>() 
                            .Input("unwrappedValue") 
                            .Build() 
                        .Build() 
                    .MissingValue(onCastFail) 
                    .Build() 
                .ElseValue<TCoNothing>() 
                    .OptionalType(returnTypeNode) 
                    .Build() 
                .Done().Ptr(); 
        }; 
 
        auto makeThrow = [&](TPositionHandle pos, const TExprNode::TPtr& message) { 
            return Build<TCoEnsure>(ctx, pos) 
                .Value<TCoNothing>() 
                    .OptionalType(ExpandType(pos, *returnTypeAnn, ctx)) 
                    .Build() 
                .Predicate<TCoBool>() 
                    .Literal() 
                        .Build("false") 
                    .Build() 
                .Message(message) 
            .Done().Ptr(); 
        }; 
 
        auto makeHandler = [&](EJsonValueHandlerMode mode, const TExprNode::TPtr& node, const TExprNode::TPtr& errorMessage, const TExprNode::TPtr& onCastFail) -> TExprNode::TPtr { 
            const auto pos = node->Pos(); 
            if (mode == EJsonValueHandlerMode::Error) { 
                return makeThrow(pos, errorMessage); 
            } 
 
            // Convert NULL to Nothing(<resultType>) 
            if (IsNull(*node)) { 
                return Build<TCoNothing>(ctx, pos) 
                        .OptionalType(ExpandType(pos, *returnTypeAnn, ctx)) 
                        .Done().Ptr(); 
            } 
 
            // If type is not Optional, wrap expression in Just call 
            TExprNode::TPtr result = node; 
            const auto typeAnn = node->GetTypeAnn(); 
            if (typeAnn->GetKind() != ETypeAnnotationKind::Optional) { 
                result = Build<TCoJust>(ctx, pos) 
                    .Input(result) 
                    .Done().Ptr(); 
            } 
 
            // Perform CAST to <resultType> or return onCastFail 
            return makeCastOrValue(pos, result, onCastFail); 
        }; 
 
        const auto onEmptyMode = FromString<EJsonValueHandlerMode>(jsonValue.OnEmptyMode().Ref().Content()); 
        const auto onErrorMode = FromString<EJsonValueHandlerMode>(jsonValue.OnErrorMode().Ref().Content()); 
        auto makeOnErrorHandler = [&](const TExprNode::TPtr& errorMessage) { 
            const auto onError = jsonValue.OnError(); 
            const auto throwCastError = makeThrow( 
                onError.Pos(), 
                Build<TCoString>(ctx, onError.Pos()) 
                    .Literal() 
                        .Build(TStringBuilder() << "Failed to cast default value from ON ERROR clause to target type " << FormatType(returnTypeAnn)) 
                .Done().Ptr() 
            ); 
 
            return makeHandler(onErrorMode, onError.Ptr(), errorMessage, throwCastError); 
        }; 
        auto makeOnEmptyHandler = [&](const TExprNode::TPtr& errorMessage) { 
            const auto onEmptyDefaultCastError = Build<TCoString>(ctx, jsonValue.OnEmpty().Pos()) 
                .Literal() 
                    .Build(TStringBuilder() << "Failed to cast default value from ON EMPTY clause to target type " << FormatType(returnTypeAnn)) 
            .Done().Ptr(); 
            return makeHandler(onEmptyMode, jsonValue.OnEmpty().Ptr(), errorMessage, makeOnErrorHandler(onEmptyDefaultCastError)); 
        }; 
 
        /* 
        Lambda for handling first type of variant 
 
        ($errorTuple) -> { 
            if $errorTuple[0] == 0 
            then 
                return onEmptyHandler 
            else 
                return onErrorHandler 
        } 
        */ 
        auto errorTupleArgument = ctx.NewArgument(jsonValuePos, "errorTuple"); 
        auto sqlValueMessage = Build<TCoNth>(ctx, jsonValuePos) 
            .Tuple(errorTupleArgument) 
            .Index() 
                .Build("1") 
            .Done().Ptr(); 
        const auto errorLambda = Build<TCoLambda>(ctx, jsonValuePos) 
            .Args(TExprNode::TListType{errorTupleArgument}) 
            .Body<TCoIf>() 
                .Predicate<TCoCmpEqual>() 
                    .Left<TCoNth>() 
                        .Tuple(errorTupleArgument) 
                        .Index() 
                            .Build("0") 
                        .Build() 
                    .Right<TCoUint8>() 
                        .Literal() 
                            .Build("0") 
                        .Build() 
                    .Build() 
                .ThenValue(makeOnEmptyHandler(sqlValueMessage)) 
                .ElseValue(makeOnErrorHandler(sqlValueMessage)) 
                .Build() 
            .Done().Ptr(); 
 
        // Lambda for handling second type of variant 
        TExprNode::TPtr sqlValueResultLambda; 
        if (needCast) { 
            const auto errorMessage = Build<TCoString>(ctx, jsonValuePos) 
                .Literal() 
                    .Build(TStringBuilder() << "Failed to cast extracted JSON value to target type " << FormatType(returnTypeAnn)) 
                .Done().Ptr(); 
            const auto inputArgument = ctx.NewArgument(jsonValuePos, "sqlValueResult"); 
            sqlValueResultLambda = Build<TCoLambda>(ctx, jsonValuePos) 
                .Args(TExprNode::TListType{inputArgument}) 
                .Body(makeCastOrValue(jsonValuePos, inputArgument, makeOnErrorHandler(errorMessage))) 
                .Done().Ptr(); 
        } else { 
            /* 
            ($sqlValueResult) -> { 
                return $sqlValueResult; 
            } 
            */ 
            sqlValueResultLambda = Build<TCoLambda>(ctx, jsonValuePos) 
                .Args({"sqlValueResult"}) 
                .Body("sqlValueResult") 
                .Done().Ptr(); 
        } 
 
        // Visit call to get the result 
        const auto visitResult = Build<TCoVisit>(ctx, jsonValuePos) 
            .Input(sqlValueExpr) 
            .FreeArgs() 
                .Add<TCoAtom>() 
                    .Build("0") 
                .Add(errorLambda) 
                .Add<TCoAtom>() 
                    .Build("1") 
                .Add(sqlValueResultLambda) 
                .Build() 
            .Done().Ptr(); 
 
        return visitResult; 
    }; 
 
    map["JsonExists"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) { 
        /* 
        Here we rewrite expression 
            JSON_EXISTS(<json expr>, <jsonpath> [PASSING <variableExpr1> AS <variableName1>, ...] {TRUE | FALSE | UNKNOWN} ON ERROR) 
        into 
            Json2::SqlExists(Json2::Parse(<json expr>), Json2::CompilePath(<jsonpath>), <dict with variables>, <on error value>) 
        and its sibling 
            JSON_EXISTS(<json expr>, <jsonpath> [PASSING <variableExpr1> AS <variableName1>, ...] ERROR ON ERROR) 
        into 
            Json2::SqlTryExists(Json2::Parse(<json expr>), <dict with variables>, Json2::CompilePath(<jsonpath>)) 
        */ 
        TCoJsonExists jsonExists(node); 
 
        // <json expr> or Json2::Parse(<json expr>) 
        EDataSlot jsonDataSlot; 
        TExprNode::TPtr parseJsonExpr = GetJsonDocumentOrParseJson(jsonExists, ctx, jsonDataSlot); 
 
        // Json2::CompilePath(<jsonPath>) 
        TExprNode::TPtr compilePathExpr = BuildJsonCompilePath(jsonExists, ctx); 
 
        // Json2::SqlExists(<json>, <compiled jsonpath>, [<default value>]) 
        // or 
        // Json2::SqlTryExists(<json>, <compiled jsonpath>) 
        const bool needThrow = !jsonExists.OnError().IsValid(); 
 
        TString sqlExistsUdfName = "SqlExists"; 
        if (needThrow) { 
            sqlExistsUdfName = "SqlTryExists"; 
        } 
 
        const TTypeAnnotationNode* inputType = nullptr; 
        if (jsonDataSlot == EDataSlot::JsonDocument) { 
            inputType = ctx.MakeType<TOptionalExprType>(ctx.MakeType<TDataExprType>(EDataSlot::JsonDocument)); 
            sqlExistsUdfName = "JsonDocument" + sqlExistsUdfName; 
        } else { 
            inputType = ctx.MakeType<TOptionalExprType>(ctx.MakeType<TResourceExprType>("JsonNode")); 
        } 
        sqlExistsUdfName = "Json2." + sqlExistsUdfName; 
 
        TTypeAnnotationNode::TListType arguments = { 
            inputType, 
            ctx.MakeType<TResourceExprType>("JsonPath") 
        }; 
 
        if (!needThrow) { 
            const auto boolType = ctx.MakeType<TDataExprType>(EDataSlot::Bool); 
            const auto optionalBoolType = ctx.MakeType<TOptionalExprType>(boolType); 
            arguments.push_back(optionalBoolType); 
        } 
 
        auto udfArgumentsType = ctx.MakeType<TTupleExprType>(TTypeAnnotationNode::TListType{ 
            ctx.MakeType<TTupleExprType>(arguments), 
            ctx.MakeType<TStructExprType>(TVector<const TItemExprType*>{}), 
            ctx.MakeType<TTupleExprType>(TTypeAnnotationNode::TListType{}) 
        }); 
 
        const auto jsonExistsPos = jsonExists.Pos(); 
        auto sqlExists = Build<TCoUdf>(ctx, jsonExistsPos) 
            .MethodName() 
                .Build(sqlExistsUdfName) 
            .RunConfigValue<TCoVoid>() 
                .Build() 
            .UserType(ExpandType(jsonExistsPos, *udfArgumentsType, ctx)) 
            .Done().Ptr(); 
 
        if (needThrow) { 
            return Build<TCoApply>(ctx, jsonExistsPos) 
                .Callable(sqlExists) 
                .FreeArgs() 
                    .Add(parseJsonExpr) 
                    .Add(compilePathExpr) 
                    .Add(jsonExists.Variables()) 
                    .Build() 
                .Done().Ptr(); 
        } 
 
        return Build<TCoApply>(ctx, jsonExistsPos) 
            .Callable(sqlExists) 
            .FreeArgs() 
                .Add(parseJsonExpr) 
                .Add(compilePathExpr) 
                .Add(jsonExists.Variables()) 
                .Add(jsonExists.OnError().Cast()) 
                .Build() 
            .Done().Ptr(); 
    }; 
 
    map["JsonQuery"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) { 
        /* 
        Here we rewrite expression 
            JSON_QUERY( 
                <json expr>, 
                <jsonpath> 
                [PASSING <variableExpr1> AS <variableName1>, ...] 
                [{WITHOUT [ARRAY] | WITH [CONDITIONAL | UNCONDITIONAL] [ARRAY]} WRAPPER] 
                [{ERROR | NULL | EMPTY ARRAY | EMPTY OBJECT} ON EMPTY] 
                [{ERROR | NULL | EMPTY ARRAY | EMPTY OBJECT} ON ERROR] 
            ) 
        into something like 
            Json2::SqlQuery...( 
                Json2::Parse(<json expr>), 
                Json2::CompilePath(<jsonpath>), 
                <dict with variables>, 
                <do we have ERROR ON EMPTY?>, 
                <default value depending on {NULL | EMPTY ARRAY | EMPTY OBJECT} ON EMPTY>, 
                <do we have ERROR ON ERROR?>, 
                <default value depending on {NULL | EMPTY ARRAY | EMPTY OBJECT} ON ERROR> 
            ) 
        Exact UDF name is choosen depending on wrap config: 
            - WITHOUT [ARRAY] WRAPPER -> Json2::SqlQuery 
            - WITH [UNCONDITIONAL] [ARRAY] WRAPPER -> Json2::SqlQueryWrap 
            - WITH CONDITIONAL [ARRAY] WRAPPER -> Json2::SqlQueryConditionalWrap 
        */ 
        TCoJsonQuery jsonQuery(node); 
 
        // <json expr> or Json2::Parse(<json expr>) 
        EDataSlot jsonDataSlot; 
        TExprNode::TPtr parseJsonExpr = GetJsonDocumentOrParseJson(jsonQuery, ctx, jsonDataSlot); 
 
        // Json2::CompilePath(<jsonPath>) 
        TExprNode::TPtr compilePathExpr = BuildJsonCompilePath(jsonQuery, ctx); 
 
        // Json2::SqlQuery...(<json expr>, <jsonpath>, ...) 
        const auto wrapMode = FromString<EJsonQueryWrap>(jsonQuery.WrapMode().Ref().Content()); 
        TString sqlQueryUdfName = "SqlQuery"; 
        switch (wrapMode) { 
            case EJsonQueryWrap::NoWrap: 
                sqlQueryUdfName = "SqlQuery"; 
                break; 
            case EJsonQueryWrap::Wrap: 
                sqlQueryUdfName = "SqlQueryWrap"; 
                break; 
            case EJsonQueryWrap::ConditionalWrap: 
                sqlQueryUdfName = "SqlQueryConditionalWrap"; 
                break; 
        } 
 
        const TTypeAnnotationNode* inputType = nullptr; 
        if (jsonDataSlot == EDataSlot::JsonDocument) { 
            inputType = ctx.MakeType<TOptionalExprType>(ctx.MakeType<TDataExprType>(EDataSlot::JsonDocument)); 
            sqlQueryUdfName = "JsonDocument" + sqlQueryUdfName; 
        } else { 
            inputType = ctx.MakeType<TOptionalExprType>(ctx.MakeType<TResourceExprType>("JsonNode")); 
        } 
        inputType = ctx.MakeType<TOptionalExprType>(inputType); 
        sqlQueryUdfName = "Json2." + sqlQueryUdfName; 
 
        const auto optionalJsonResourceType = ctx.MakeType<TOptionalExprType>(ctx.MakeType<TResourceExprType>("JsonNode")); 
        TTypeAnnotationNode::TListType arguments{ 
            inputType, 
            ctx.MakeType<TResourceExprType>("JsonPath"), 
            ctx.MakeType<TDataExprType>(EDataSlot::Bool), 
            optionalJsonResourceType, 
            ctx.MakeType<TDataExprType>(EDataSlot::Bool), 
            optionalJsonResourceType, 
        }; 
 
        auto udfArgumentsType = ctx.MakeType<TTupleExprType>(TTypeAnnotationNode::TListType{ 
            ctx.MakeType<TTupleExprType>(arguments), 
            ctx.MakeType<TStructExprType>(TVector<const TItemExprType*>{}), 
            ctx.MakeType<TTupleExprType>(TTypeAnnotationNode::TListType{}) 
        }); 
 
        auto buildShouldThrow = [&](EJsonQueryHandler handler, TPositionHandle pos) { 
            return Build<TCoBool>(ctx, pos) 
                    .Literal() 
                        .Build(handler == EJsonQueryHandler::Error ? "true" : "false") 
                    .Done().Ptr(); 
        }; 
 
        auto buildHandler = [&](EJsonQueryHandler handler, TPositionHandle pos) { 
            switch (handler) { 
                case EJsonQueryHandler::Error: 
                case EJsonQueryHandler::Null: { 
                    // Nothing(Resource<JsonNode>) 
                    return Build<TCoNothing>(ctx, pos) 
                            .OptionalType(ExpandType(pos, *optionalJsonResourceType, ctx)) 
                            .Done().Ptr(); 
                } 
                case EJsonQueryHandler::EmptyArray: { 
                    auto value = Build<TCoJson>(ctx, pos) 
                        .Literal() 
                            .Build("[]") 
                        .Done().Ptr(); 
                    return BuildJsonParse(value, ctx); 
                } 
                case EJsonQueryHandler::EmptyObject: { 
                    auto value = Build<TCoJson>(ctx, pos) 
                        .Literal() 
                            .Build("{}") 
                        .Done().Ptr(); 
                    return BuildJsonParse(value, ctx); 
                } 
            } 
        }; 
 
        const auto jsonQueryPos = jsonQuery.Pos(); 
        auto sqlQuery = Build<TCoUdf>(ctx, jsonQueryPos) 
            .MethodName() 
                .Build(sqlQueryUdfName) 
            .RunConfigValue<TCoVoid>() 
                .Build() 
            .UserType(ExpandType(jsonQueryPos, *udfArgumentsType, ctx)) 
            .Done().Ptr(); 
 
        const auto onEmpty = FromString<EJsonQueryHandler>(jsonQuery.OnEmpty().Ref().Content()); 
        const auto onError = FromString<EJsonQueryHandler>(jsonQuery.OnError().Ref().Content()); 
        const auto onEmptyPos = jsonQuery.OnEmpty().Pos(); 
        const auto onErrorPos = jsonQuery.OnError().Pos(); 
 
        auto sqlQueryApply = Build<TCoApply>(ctx, jsonQueryPos) 
            .Callable(sqlQuery) 
            .FreeArgs() 
                .Add(parseJsonExpr) 
                .Add(compilePathExpr) 
                .Add(jsonQuery.Variables()) 
                .Add(buildShouldThrow(onEmpty, onEmptyPos)) 
                .Add(buildHandler(onEmpty, onEmptyPos)) 
                .Add(buildShouldThrow(onError, onErrorPos)) 
                .Add(buildHandler(onError, onErrorPos)) 
                .Build() 
            .Done().Ptr(); 
 
        // In this case we need to serialize Resource<JsonNode> to Json type 
        if (!optCtx.Types->JsonQueryReturnsJsonDocument) { 
            return BuildJsonSerialize(sqlQueryApply, ctx); 
        } 
 
        // Now we need to serialize Resource<JsonNode> from sqlQueryApply to JsonDocument 
        { 
            auto resourcePos = sqlQueryApply->Pos(); 
 
            auto argumentsType = ctx.MakeType<TTupleExprType>(TTypeAnnotationNode::TListType{ 
                ctx.MakeType<TOptionalExprType>(ctx.MakeType<TResourceExprType>("JsonNode")), 
            }); 
 
            auto udfArgumentsType = ctx.MakeType<TTupleExprType>(TTypeAnnotationNode::TListType{ 
                argumentsType, 
                ctx.MakeType<TStructExprType>(TVector<const TItemExprType*>{}), 
                ctx.MakeType<TTupleExprType>(TTypeAnnotationNode::TListType{}) 
            }); 
 
            TStringBuf serializeUdfName = "Json2.Serialize"; 
            if (optCtx.Types->JsonQueryReturnsJsonDocument) { 
                serializeUdfName = "Json2.SerializeToJsonDocument"; 
            } 
            auto parse = Build<TCoUdf>(ctx, resourcePos) 
                .MethodName() 
                    .Build(serializeUdfName) 
                .RunConfigValue<TCoVoid>() 
                    .Build() 
                .UserType(ExpandType(resourcePos, *udfArgumentsType, ctx)) 
                .Done().Ptr(); 
 
            return Build<TCoApply>(ctx, resourcePos) 
                .Callable(parse) 
                .FreeArgs() 
                    .Add(sqlQueryApply) 
                    .Build() 
                .Done().Ptr(); 
        } 
    }; 
 
    map["JsonVariables"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) { 
        /* 
        Here we rewrite expression 
            PASSING 
                <expr1> as <name1>, 
                <expr2> as <name2>, 
                ... 
        Into something like: 
            AsDict( 
                '(  <-- tuple creation 
                    <name1>, 
                    Json2::...AsJsonNode(<expr1>)  <-- exact name depends on the <expr1> type 
                ), 
                '( 
                    <name2>, 
                    Json2::...AsJsonNode(<expr2>) 
                ), 
                .... 
            ) 
        If <expr> is NULL, it is replaced with Nothing(String). 
        If <expr> is not Optional, it is wrapped in Just call. 
        */ 
        TCoJsonVariables jsonVariables(node); 
        const auto pos = jsonVariables.Pos(); 
 
        TVector<TExprNode::TPtr> children; 
        for (const auto& tuple : jsonVariables) { 
            TExprNode::TPtr name = tuple.Name().Ptr(); 
            const auto nameUtf8 = Build<TCoUtf8>(ctx, name->Pos()) 
                .Literal(name) 
                .Done().Ptr(); 
 
            TExprNode::TPtr payload = tuple.Value().Cast().Ptr(); 
            auto argumentsType = ctx.MakeType<TTupleExprType>(TTypeAnnotationNode::TListType{ 
                payload->GetTypeAnn(), 
            }); 
 
            auto udfArgumentsType = ctx.MakeType<TTupleExprType>(TTypeAnnotationNode::TListType{ 
                argumentsType, 
                ctx.MakeType<TStructExprType>(TVector<const TItemExprType*>{}), 
                ctx.MakeType<TTupleExprType>(TTypeAnnotationNode::TListType{}) 
            }); 
 
            EDataSlot payloadSlot; 
            const auto* payloadType = payload->GetTypeAnn(); 
            if (payloadType->GetKind() == ETypeAnnotationKind::Null) { 
                // we treat NULL as Nothing(Utf8?) 
                payloadSlot = EDataSlot::Utf8; 
                const auto* optionalUtf8 = ctx.MakeType<TOptionalExprType>(ctx.MakeType<TDataExprType>(payloadSlot)); 
                payload = Build<TCoNothing>(ctx, pos) 
                    .OptionalType(ExpandType(pos, *optionalUtf8, ctx)) 
                    .Done().Ptr(); 
            } else if (payloadType->GetKind() == ETypeAnnotationKind::Optional) { 
                payloadSlot = payloadType->Cast<TOptionalExprType>()->GetItemType()->Cast<TDataExprType>()->GetSlot(); 
            } else { 
                payloadSlot = payloadType->Cast<TDataExprType>()->GetSlot(); 
                payload = Build<TCoJust>(ctx, pos) 
                    .Input(payload) 
                    .Done().Ptr(); 
            } 
 
            TStringBuf convertUdfName; 
            if (IsDataTypeNumeric(payloadSlot) || IsDataTypeDate(payloadSlot)) { 
                payload = Build<TCoSafeCast>(ctx, pos) 
                        .Value(payload) 
                        .Type(ExpandType(payload->Pos(), *ctx.MakeType<TDataExprType>(EDataSlot::Double), ctx)) 
                    .Done().Ptr(); 
                convertUdfName = "Json2.DoubleAsJsonNode"; 
            } else if (payloadSlot == EDataSlot::Utf8) { 
                convertUdfName = "Json2.Utf8AsJsonNode"; 
            } else if (payloadSlot == EDataSlot::Bool) { 
                convertUdfName = "Json2.BoolAsJsonNode"; 
            } else if (payloadSlot == EDataSlot::Json) { 
                convertUdfName = "Json2.JsonAsJsonNode"; 
            } else { 
                YQL_ENSURE(false, "Unsupported type"); 
            } 
 
            auto payloadPos = payload->Pos(); 
            auto convert = Build<TCoUdf>(ctx, payloadPos) 
                .MethodName() 
                    .Build(convertUdfName) 
                .RunConfigValue<TCoVoid>() 
                    .Build() 
                .UserType(ExpandType(payloadPos, *udfArgumentsType, ctx)) 
                .Done().Ptr(); 
 
            auto applyConvert = Build<TCoApply>(ctx, payloadPos) 
                .Callable(convert) 
                .FreeArgs() 
                    .Add(payload) 
                    .Build() 
                .Done().Ptr(); 
 
            auto pair = ctx.NewList(tuple.Pos(), {nameUtf8, applyConvert}); 
            children.push_back(pair); 
        } 
 
        return Build<TCoAsDict>(ctx, pos) 
            .FreeArgs() 
                .Add(children) 
                .Build() 
            .Done().Ptr(); 
    }; 

    map["CalcOverWindow"] = map["CalcOverSessionWindow"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        TCoCalcOverWindowBase self(node);
        auto frames = self.Frames();
        size_t sessionColumnsSize = 0;
        if (auto maybeSession = TMaybeNode<TCoCalcOverSessionWindow>(node)) {
            sessionColumnsSize = maybeSession.Cast().SessionColumns().Size();
        }
        if (frames.Size() == 0 && sessionColumnsSize == 0) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " without payload";
            return self.Input().Ptr();
        }

        auto mergedFrames = MergeCalcOverWindowFrames(frames.Ptr(), ctx);
        if (mergedFrames == frames.Ptr()) {
            return node;
        }

        YQL_CLOG(DEBUG, Core) << node->Content() << " with duplicate or empty frames";
        return ctx.ChangeChild(*node, TCoCalcOverWindowBase::idx_Frames, std::move(mergedFrames));
    };

    map["CalcOverWindowGroup"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        TCoCalcOverWindowGroup self(node);

        auto dedupCalcs = DedupCalcOverWindowsOnSamePartitioning(self.Calcs().Ref().ChildrenList(), ctx);
        YQL_ENSURE(dedupCalcs.size() <= self.Calcs().Size());

        TExprNodeList mergedCalcs;
        bool merged = false;
        for (auto& calcNode : dedupCalcs) {
            TCoCalcOverWindowTuple calc(calcNode);

            auto origFrames = calc.Frames().Ptr();
            auto mergedFrames = MergeCalcOverWindowFrames(origFrames, ctx);
            if (mergedFrames != origFrames) {
                merged = true;
                mergedCalcs.emplace_back(
                    Build<TCoCalcOverWindowTuple>(ctx, calc.Pos())
                        .Keys(calc.Keys())
                        .SortSpec(calc.SortSpec())
                        .Frames(mergedFrames)
                        .SessionSpec(calc.SessionSpec())
                        .SessionColumns(calc.SessionColumns())
                        .Done().Ptr()
                );
            } else {
                mergedCalcs.emplace_back(std::move(calcNode));
            }
        }

        if (merged || dedupCalcs.size() < self.Calcs().Size()) {
            YQL_CLOG(DEBUG, Core) << "CalcOverWindowGroup with duplicate/empty frames and/or duplicate windows";
            return BuildCalcOverWindowGroup(self, std::move(mergedCalcs), ctx);
        }

        if (mergedCalcs.size() <= 1) {
            TStringBuf msg = mergedCalcs.empty() ? "CalcOverWindowGroup without windows" : "CalcOverWindowGroup with single window";
            YQL_CLOG(DEBUG, Core) << msg;
            return BuildCalcOverWindowGroup(self, std::move(mergedCalcs), ctx);
        }

        return node;
    };

    map["AssumeColumnOrder"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        auto input = node->HeadPtr();
        if (input->IsCallable("AssumeColumnOrder")) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << input->Content();
            return ctx.ChangeChild(*node, 0u, input->HeadPtr());
        }

        return node;
    };

    map["SqlProject"] = map["OrderedSqlProject"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) {
        YQL_CLOG(DEBUG, Core) << "Expand " << node->Content();

        TExprNodeList lambdas;
        for (auto& item : node->Child(1)->Children()) {
            YQL_ENSURE(item->IsCallable({"SqlProjectItem", "SqlProjectStarItem"}));
            YQL_ENSURE(item->Child(1)->IsAtom());
            YQL_ENSURE(item->Child(2)->IsLambda());
            if (item->IsCallable("SqlProjectStarItem")) {
                lambdas.push_back(item->ChildPtr(2));
            } else {
                auto targetName = item->Child(1)->Content();
                lambdas.push_back(
                    ctx.Builder(item->Pos())
                        .Lambda()
                            .Param("row")
                            .Callable("AsStruct")
                                .List(0)
                                    .Atom(0, targetName)
                                    .Apply(1, item->ChildPtr(2))
                                        .With(0, "row")
                                    .Seal()
                                .Seal()
                            .Seal()
                        .Seal()
                        .Build());
            }
        }

        auto res = ctx.Builder(node->Pos())
            .Callable(node->IsCallable("SqlProject") ? "FlatMap" : "OrderedFlatMap")
                .Add(0, node->ChildPtr(0))
                .Lambda(1)
                    .Param("row")
                    .Callable("AsList")
                        .Callable(0, "FlattenMembers")
                            .Do([&](TExprNodeBuilder& parent) -> TExprNodeBuilder& {
                                size_t index = 0;
                                for (auto lambda: lambdas) {
                                    parent
                                        .List(index++)
                                            .Atom(0, "")
                                            .Apply(1, lambda)
                                                .With(0, "row")
                                            .Seal()
                                        .Seal();
                                }
                                return parent;
                            })
                        .Seal()
                    .Seal()
                .Seal()
            .Seal()
            .Build();

        return KeepColumnOrder(res, *node, ctx, *optCtx.Types);
    };

    map["SqlFlattenByColumns"] = map["OrderedSqlFlattenByColumns"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) {
        YQL_CLOG(DEBUG, Core) << "Expand " << node->Content();

        auto row = ctx.NewArgument(node->Pos(), "row");

        auto flattenByArgs = node->ChildrenList();
        flattenByArgs[0] = flattenByArgs[1];
        flattenByArgs[1] = row;

        auto body = ctx.NewCallable(node->Pos(), "FlattenByColumns", std::move(flattenByArgs));

        auto res = ctx.Builder(node->Pos())
            .Callable(node->Content().StartsWith("Ordered") ? "OrderedFlatMap" : "FlatMap")
                .Add(0, node->HeadPtr())
                .Add(1, ctx.NewLambda(node->Pos(), ctx.NewArguments(node->Pos(), { row }), std::move(body)))
            .Seal()
            .Build();
        return KeepColumnOrder(res, *node, ctx, *optCtx.Types);
    };

    map["SqlFlattenColumns"] = map["OrderedSqlFlattenColumns"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) {
        YQL_CLOG(DEBUG, Core) << "Expand " << node->Content();

        auto res = ctx.Builder(node->Pos())
            .Callable(node->Content().StartsWith("Ordered") ? "OrderedFlatMap" : "FlatMap")
                .Add(0, node->HeadPtr())
                .Lambda(1)
                    .Param("row")
                    .Callable("Just")
                        .Callable(0, "FlattenStructs")
                            .Arg(0, "row")
                        .Seal()
                    .Seal()
                .Seal()
            .Seal()
            .Build();
        return KeepColumnOrder(res, *node, ctx, *optCtx.Types);
    };

    map["SqlAggregateAll"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) {
        YQL_CLOG(DEBUG, Core) << "Expand " << node->Content();
        auto voidNode = ctx.NewCallable(node->Pos(), "Void", {});
        auto emptyTuple = ctx.NewList(node->Pos(), {});
        auto res = ctx.NewCallable(node->Pos(), "Aggregate", { node->HeadPtr(), voidNode, emptyTuple, emptyTuple});
        return KeepColumnOrder(res, *node, ctx, *optCtx.Types);
    };

    map["Mux"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) {
        if (node->Head().IsList()) {
            TExprNodeList children = node->Head().ChildrenList();
            bool found = false;
            for (auto& child : children) {
                if (child->IsCallable("AssumeColumnOrder")) {
                    found = true;
                    child = child->HeadPtr();
                }
            }

            if (found) {
                YQL_CLOG(DEBUG, Core) << "Pull AssumeColumnOrder over " << node->Content();
                auto res = ctx.ChangeChild(*node, 0, ctx.NewList(node->Pos(), std::move(children)));
                return KeepColumnOrder(res, *node, ctx, *optCtx.Types);
            }
        }

        return node;
    };

    map["UnionAllPositional"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) {
        YQL_CLOG(DEBUG, Core) << "Expand " << node->Content();
        if (node->ChildrenSize() == 1) {
            return node->HeadPtr();
        }

        TVector<TColumnOrder> columnOrders;
        for (auto child : node->Children()) {
            auto childColumnOrder = optCtx.Types->LookupColumnOrder(*child);
            YQL_ENSURE(childColumnOrder);
            columnOrders.push_back(*childColumnOrder);
        }

        return ExpandPositionalUnionAll(*node, columnOrders, node->ChildrenList(), ctx, optCtx);
    };

    map["MapJoinCore"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) {
        if (const auto& inputToCheck = SkipCallables(node->Head(), SkippableCallables); IsEmptyContainer(inputToCheck) || IsEmpty(inputToCheck, *optCtx.Types)) {
            YQL_CLOG(DEBUG, Core) << "Empty " << node->Content();
            return ctx.NewCallable(inputToCheck.Pos(), "EmptyIterator", {ExpandType(node->Pos(), *node->GetTypeAnn(), ctx)});
        }

        if (const TCoMapJoinCore mapJoin(node); IsEmptyContainer(mapJoin.RightDict().Ref())) {
            YQL_CLOG(DEBUG, Core) << node->Content() << " with empty " << mapJoin.RightDict().Ref().Content();

            if (const auto& joinKind = mapJoin.JoinKind().Value(); joinKind == "Inner" || joinKind == "LeftSemi")
                return ctx.NewCallable(mapJoin.Pos(), "EmptyIterator", {ExpandType(mapJoin.Pos(), *node->GetTypeAnn(), ctx)});
            else if (joinKind == "Left" || joinKind == "LeftOnly") {
                switch (const auto itemType = GetSeqItemType(node->GetTypeAnn()); itemType->GetKind()) {
                    case ETypeAnnotationKind::Tuple: {
                        const auto& items = itemType->Cast<TTupleExprType>()->GetItems();
                        auto row = ctx.NewArgument(mapJoin.Pos(), "row");
                        TExprNode::TListType fields(items.size());
                        for (auto i = 1U; i < mapJoin.LeftRenames().Size(); ++++i) {
                            const auto index = FromString<ui32>(mapJoin.LeftRenames().Item(i).Value());
                            fields[index] = ctx.Builder(mapJoin.LeftRenames().Item(i).Pos())
                                .Callable("Nth")
                                    .Add(0, row)
                                    .Add(1, mapJoin.LeftRenames().Item(i - 1U).Ptr())
                                .Seal().Build();
                        }
                        for (auto i = 1U; i < mapJoin.RightRenames().Size(); ++++i) {
                            const auto index = FromString<ui32>(mapJoin.RightRenames().Item(i).Value());
                            fields[index] = ctx.Builder(mapJoin.RightRenames().Item(i).Pos())
                                .Callable("Nothing")
                                    .Add(0, ExpandType(mapJoin.Pos(), *items[index], ctx))
                                .Seal().Build();
                        }
                        auto lambda = ctx.NewLambda(mapJoin.Pos(), ctx.NewArguments(mapJoin.Pos(), {std::move(row)}), ctx.NewList(mapJoin.Pos(), std::move(fields)));
                        return ctx.NewCallable(mapJoin.Pos(), "Map", {mapJoin.LeftInput().Ptr(), std::move(lambda)});
                    }
                    case ETypeAnnotationKind::Struct: {
                        const auto structType = itemType->Cast<TStructExprType>();
                        const auto& items = structType->GetItems();
                        auto row = ctx.NewArgument(mapJoin.Pos(), "row");
                        TExprNode::TListType fields(items.size());
                        for (auto i = 1U; i < mapJoin.LeftRenames().Size(); ++++i) {
                            const auto index = *structType->FindItem(mapJoin.LeftRenames().Item(i).Value());
                            fields[index] = ctx.Builder(mapJoin.LeftRenames().Item(i).Pos())
                                .List()
                                    .Add(0, mapJoin.LeftRenames().Item(i).Ptr())
                                    .Callable(1, "Member")
                                        .Add(0, row)
                                        .Add(1, mapJoin.LeftRenames().Item(i - 1U).Ptr())
                                    .Seal()
                                .Seal().Build();
                        }
                        for (auto i = 1U; i < mapJoin.RightRenames().Size(); ++++i) {
                            const auto index = *structType->FindItem(mapJoin.RightRenames().Item(i).Value());
                            fields[index] = ctx.Builder(mapJoin.RightRenames().Item(i).Pos())
                                .List()
                                    .Add(0, mapJoin.RightRenames().Item(i).Ptr())
                                    .Callable(1, "Nothing")
                                        .Add(0, ExpandType(mapJoin.Pos(), *items[index]->GetItemType(), ctx))
                                    .Seal()
                                .Seal().Build();
                        }
                        auto lambda = ctx.NewLambda(mapJoin.Pos(), ctx.NewArguments(mapJoin.Pos(), {std::move(row)}), ctx.NewCallable(mapJoin.Pos(), "AsStruct", std::move(fields)));
                        return ctx.NewCallable(mapJoin.Pos(), "Map", {mapJoin.LeftInput().Ptr(), std::move(lambda)});
                    }
                    default: break;
                }
            }
        }

        return node;
    };

    map["RangeIntersect"] = [](const TExprNode::TPtr& node, TExprContext& /*ctx*/, TOptimizeContext& /*optCtx*/) {
        if (node->ChildrenSize() == 1) {
            YQL_CLOG(DEBUG, Core) << "Single arg " << node->Content();
            return node->HeadPtr();
        }
        return node;
    };

    map["RangeUnion"] = [](const TExprNode::TPtr& node, TExprContext& /*ctx*/, TOptimizeContext& /*optCtx*/) {
        if (node->ChildrenSize() == 1) {
            if (node->Head().IsCallable("RangeUnion")) {
                YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
                return node->HeadPtr();
            }

            if (node->Head().IsCallable("RangeMultiply")) {
                auto children = node->Head().ChildrenList();
                YQL_ENSURE(children.size() > 1);
                if (AllOf(children.begin() + 1, children.end(), [](const auto& child) { return child->IsCallable("RangeUnion"); })) {
                    YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Head().Content();
                    return node->HeadPtr();
                }
            }
        }
        return node;
    };

    map["RangeMultiply"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        if (node->ChildrenSize() == 2 && node->Tail().IsCallable("RangeMultiply")) {
            auto minLimit = ctx.NewCallable(node->Pos(), "Min", { node->HeadPtr(), node->Tail().HeadPtr() });
            YQL_CLOG(DEBUG, Core) << node->Content() << " over " << node->Tail().Content();
            return ctx.ChangeChild(node->Tail(), 0, std::move(minLimit));
        }
        return node;
    };

    map["PgSelect"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) -> TExprNode::TPtr {
        auto setItems = GetSetting(node->Head(), "set_items");
        auto order = optCtx.Types->LookupColumnOrder(*node);
        YQL_ENSURE(order);
        TExprNode::TListType columnsItems;
        for (const auto& x : *order) {
            columnsItems.push_back(ctx.NewAtom(node->Pos(), x));
        }

        auto columns = ctx.NewList(node->Pos(), std::move(columnsItems));
        TExprNode::TListType setItemNodes;
        TVector<TColumnOrder> columnOrders;
        for (auto setItem : setItems->Tail().Children()) {
            auto childOrder = optCtx.Types->LookupColumnOrder(*setItem);
            YQL_ENSURE(*childOrder);
            columnOrders.push_back(*childOrder);
            auto result = GetSetting(setItem->Tail(), "result");
            auto values = GetSetting(setItem->Tail(), "values");
            auto from = GetSetting(setItem->Tail(), "from");
            auto filter = GetSetting(setItem->Tail(), "where");
            auto joinOps = GetSetting(setItem->Tail(), "join_ops");
            auto groupBy = GetSetting(setItem->Tail(), "group_by");
            auto having = GetSetting(setItem->Tail(), "having");
            auto window = GetSetting(setItem->Tail(), "window");
            bool oneRow = !from;
            TExprNode::TPtr list;
            if (values) {
                YQL_ENSURE(!result);
                list = ctx.Builder(node->Pos())
                    .Callable("Map")
                        .Add(0, values->ChildPtr(2))
                        .Lambda(1)
                            .Param("row")
                            .Callable("AsStruct")
                            .Do([&](TExprNodeBuilder& parent) -> TExprNodeBuilder& {
                                for (ui32 index = 0; index < values->Child(1)->ChildrenSize(); ++index) {
                                    parent
                                        .List(index)
                                            .Atom(0, values->Child(1)->Child(index)->Content())
                                            .Callable(1, "Nth")
                                                .Arg(0, "row")
                                                .Atom(1, ToString(index))
                                            .Seal()
                                        .Seal();
                                }

                                return parent;
                            })
                            .Seal()
                        .Seal()
                    .Seal()
                    .Build();
            } else {
                YQL_ENSURE(result);
                TExprNode::TPtr projectionLambda;
                if (oneRow) {
                    TExprNode::TListType rowItems;
                    for (const auto& x : result->Tail().Children()) {
                        rowItems.push_back(ctx.NewList(x->Pos(), { x->HeadPtr(), x->Tail().TailPtr() }));
                    }

                    auto row = ctx.NewCallable(node->Pos(), "AsStruct", std::move(rowItems));
                    list = ctx.Builder(node->Pos())
                        .Callable("AsList")
                            .Callable(0, "AsStruct")
                            .Seal()
                        .Seal()
                        .Build();

                    projectionLambda = ctx.Builder(node->Pos())
                        .Lambda()
                            .Param("row")
                            .Set(row)
                            .Seal()
                        .Build();
                } else {
                    // extract all used columns
                    TMap<TString,std::pair<ui32, TString>> usedColumns;
                    for (const auto& x : result->Tail().Children()) {
                        auto type = x->Child(1)->GetTypeAnn()->Cast<TTypeExprType>()->GetType()->Cast<TStructExprType>();
                        for (auto item : type->GetItems()) {
                            usedColumns.insert(std::make_pair(TString(item->GetName()), std::make_pair(Max<ui32>(), TString())));
                        }
                    }

                    for (ui32 groupNo = 0; groupNo < joinOps->Tail().ChildrenSize(); ++groupNo) {
                        auto groupTuple = joinOps->Tail().Child(groupNo);
                        for (ui32 i = 0; i < groupTuple->ChildrenSize(); ++i) {
                            auto join = groupTuple->Child(i);
                            auto joinType = join->Child(0)->Content();
                            if (joinType != "cross") {
                                auto type = join->Tail().Child(0)->GetTypeAnn()->Cast<TTypeExprType>()->GetType()->Cast<TStructExprType>();
                                for (auto item : type->GetItems()) {
                                    usedColumns.insert(std::make_pair(TString(item->GetName()), std::make_pair(Max<ui32>(), TString())));
                                }
                            }
                        }
                    }

                    // fill index of input for each column
                    for (auto& x : usedColumns) {
                        bool foundColumn = false;
                        for (ui32 inputIndex = 0; inputIndex < from->Tail().ChildrenSize(); ++inputIndex) {
                            const auto& read = from->Tail().Child(inputIndex)->Head();
                            const auto& columns = from->Tail().Child(inputIndex)->Tail();
                            if (columns.ChildrenSize() > 0) {
                                auto readOrder = optCtx.Types->LookupColumnOrder(read);
                                YQL_ENSURE(*readOrder);
                                for (ui32 i = 0; i < columns.ChildrenSize(); ++i) {
                                    if (columns.Child(i)->Content() == x.first) {
                                        foundColumn = true;
                                        x.second.second = (*readOrder)[i];
                                        break;
                                    }
                                }
                            } else {
                                auto type = read.GetTypeAnn()->
                                    Cast<TListExprType>()->GetItemType()->Cast<TStructExprType>();
                                auto pos = type->FindItem(x.first);
                                foundColumn = pos.Defined();
                            }

                            if (foundColumn) {
                                x.second.first = inputIndex;
                                break;
                            }
                        }

                        YQL_ENSURE(foundColumn, "Missing column: " << x.first);
                    }

                    TVector<TExprNode::TPtr> cleanedInputs;
                    for (ui32 i = 0; i < from->Tail().ChildrenSize(); ++i) {
                        auto cleaned = ctx.Builder(node->Pos())
                            .Callable("OrderedMap")
                                .Add(0, from->Tail().Child(i)->HeadPtr())
                                .Lambda(1)
                                    .Param("row")
                                    .Callable("AsStruct")
                                        .Do([&](TExprNodeBuilder& parent) -> TExprNodeBuilder& {
                                            ui32 index = 0;
                                            for (const auto& x : usedColumns) {
                                                if (x.second.first != i) {
                                                    continue;
                                                }

                                                auto listBuilder = parent.List(index++);
                                                listBuilder.Atom(0, x.first);
                                                listBuilder.Callable(1, "Member")
                                                    .Arg(0, "row")
                                                    .Atom(1, x.second.second ? x.second.second : x.first)
                                                .Seal();
                                                listBuilder.Seal();
                                            }

                                            return parent;
                                        })
                                    .Seal()
                                .Seal()
                            .Seal()
                            .Build();

                        cleanedInputs.push_back(cleaned);
                    }

                    if (cleanedInputs.size() == 1) {
                        list = cleanedInputs.front();
                    } else {
                        TVector<ui32> groupForIndex;
                        TExprNode::TListType joinGroups;
                        ui32 inputIndex = 0;
                        for (ui32 groupNo = 0; groupNo < joinOps->Tail().ChildrenSize(); ++groupNo) {
                            groupForIndex.push_back(groupNo);
                            auto groupTuple = joinOps->Tail().Child(groupNo);
                            if (groupTuple->ChildrenSize() == 0) {
                                joinGroups.push_back(cleanedInputs[inputIndex++]);
                                continue;
                            }

                            auto current = cleanedInputs[inputIndex++];
                            for (ui32 i = 0; i < groupTuple->ChildrenSize(); ++i) {
                                groupForIndex.push_back(groupNo);
                                auto with = cleanedInputs[inputIndex++];
                                // current = join current & with
                                auto join = groupTuple->Child(i);
                                auto joinType = join->Child(0)->Content();
                                auto cartesian = ctx.Builder(node->Pos())
                                        .Callable("OrderedFlatMap")
                                            .Add(0, current)
                                            .Lambda(1)
                                                .Param("x")
                                                .Callable("Map")
                                                    .Add(0, with)
                                                    .Lambda(1)
                                                        .Param("y")
                                                        .Callable("FlattenMembers")
                                                            .List(0)
                                                                .Atom(0, "")
                                                                .Arg(1,"x")
                                                            .Seal()
                                                            .List(1)
                                                                .Atom(0, "")
                                                                .Arg(1, "y")
                                                            .Seal()
                                                        .Seal()
                                                    .Seal()
                                                .Seal()
                                            .Seal()
                                        .Seal()
                                        .Build();

                                auto buildMinus = [&](auto left, auto right) {
                                    return ctx.Builder(node->Pos())
                                        .Callable("OrderedFlatMap")
                                            .Add(0, left)
                                            .Lambda(1)
                                                .Param("x")
                                                .Callable("OptionalIf")
                                                    .Callable(0, "Not")
                                                        .Callable(0, "HasItems")
                                                            .Callable(0, "Filter")
                                                                .Add(0, right)
                                                                .Lambda(1)
                                                                    .Param("y")
                                                                    .Apply(join->Tail().TailPtr())
                                                                        .With(0)
                                                                            .Callable("FlattenMembers")
                                                                                .List(0)
                                                                                    .Atom(0, "")
                                                                                    .Arg(1,"x")
                                                                                .Seal()
                                                                                .List(1)
                                                                                    .Atom(0, "")
                                                                                    .Arg(1, "y")
                                                                                .Seal()
                                                                            .Seal()
                                                                        .Done()
                                                                    .Seal()
                                                                .Seal()
                                                            .Seal()
                                                        .Seal()
                                                    .Seal()
                                                    .Arg(1, "x")
                                                .Seal()
                                            .Seal()
                                        .Seal()
                                        .Build();
                                };

                                TExprNode::TPtr filteredCartesian;
                                if (joinType != "cross") {
                                    filteredCartesian = ctx.Builder(node->Pos())
                                        .Callable("OrderedFilter")
                                            .Add(0, cartesian)
                                            .Lambda(1)
                                                .Param("row")
                                                .Apply(join->Tail().TailPtr())
                                                    .With(0, "row")
                                                .Seal()
                                            .Seal()
                                        .Seal()
                                        .Build();
                                }

                                if (joinType == "cross") {
                                    current = cartesian;
                                } else if (joinType == "inner") {
                                    current = filteredCartesian;
                                } else if (joinType == "left") {
                                    current = ctx.Builder(node->Pos())
                                        .Callable("UnionAll")
                                            .Add(0, filteredCartesian)
                                            .Add(1, buildMinus(current, with))
                                        .Seal()
                                        .Build();
                                } else if (joinType == "right") {
                                    current = ctx.Builder(node->Pos())
                                        .Callable("UnionAll")
                                            .Add(0, filteredCartesian)
                                            .Add(1, buildMinus(with, current))
                                        .Seal()
                                        .Build();
                                } else {
                                    YQL_ENSURE(joinType == "full");
                                    current = ctx.Builder(node->Pos())
                                        .Callable("UnionAll")
                                            .Add(0, filteredCartesian)
                                            .Add(1, buildMinus(current, with))
                                            .Add(2, buildMinus(with, current))
                                        .Seal()
                                        .Build();
                                }
                            }

                            joinGroups.push_back(current);
                        }

                        if (joinGroups.size() == 1) {
                            list = joinGroups.front();
                        } else {
                            TExprNode::TListType args;
                            for (ui32 i = 0; i < joinGroups.size(); ++i) {
                                args.push_back(ctx.Builder(node->Pos())
                                    .List()
                                        .Add(0, joinGroups[i])
                                        .Atom(1, ToString(i))
                                    .Seal()
                                    .Build());
                            }

                            auto tree = ctx.Builder(node->Pos())
                                .List()
                                    .Atom(0, "Cross")
                                    .Atom(1, "0")
                                    .Atom(2, "1")
                                    .List(3)
                                    .Seal()
                                    .List(4)
                                    .Seal()
                                    .List(5)
                                    .Seal()
                                .Seal()
                                .Build();

                            for (ui32 i = 2; i < joinGroups.size(); ++i) {
                                tree = ctx.Builder(node->Pos())
                                    .List()
                                        .Atom(0, "Cross")
                                        .Add(1, tree)
                                        .Atom(2, ToString(i))
                                        .List(3)
                                        .Seal()
                                        .List(4)
                                        .Seal()
                                        .List(5)
                                        .Seal()
                                    .Seal()
                                    .Build();
                            }

                            args.push_back(tree);
                            TExprNode::TListType settings;
                            for (const auto& x : usedColumns) {
                                settings.push_back(ctx.Builder(node->Pos())
                                    .List()
                                        .Atom(0, "rename")
                                        .Atom(1, ToString(groupForIndex[x.second.first]) + "." + x.first)
                                        .Atom(2, x.first)
                                    .Seal()
                                    .Build());
                            }

                            auto settingsNode = ctx.NewList(node->Pos(), std::move(settings));
                            args.push_back(settingsNode);
                            list = ctx.NewCallable(node->Pos(), "EquiJoin", std::move(args));
                        }
                    }

                    projectionLambda = ctx.Builder(node->Pos())
                        .Lambda()
                            .Param("row")
                            .Callable("AsStruct")
                            .Do([&](TExprNodeBuilder& parent) -> TExprNodeBuilder& {
                                ui32 index = 0;
                                for (const auto& x : result->Tail().Children()) {
                                    if (x->HeadPtr()->IsAtom()) {
                                        auto listBuilder = parent.List(index++);
                                        listBuilder.Add(0, x->HeadPtr());
                                        listBuilder.Apply(1, x->TailPtr())
                                            .With(0, "row")
                                        .Seal();
                                        listBuilder.Seal();
                                    } else {
                                        for (ui32 i = 0; i < x->Head().ChildrenSize(); ++i) {
                                            auto listBuilder = parent.List(index++);
                                            listBuilder.Add(0, x->Head().ChildPtr(i));
                                            listBuilder.Callable(1, "Member")
                                                .Arg(0, "row")
                                                .Add(1, x->Head().ChildPtr(i));
                                            listBuilder.Seal();
                                        }
                                    }
                                }

                                return parent;
                            })
                            .Seal()
                        .Seal()
                        .Build();
                }

                if (filter) {
                    list = ctx.Builder(node->Pos())
                        .Callable("Filter")
                            .Add(0, list)
                            .Lambda(1)
                                .Param("row")
                                .Apply(filter->Tail().TailPtr())
                                    .With(0, "row")
                                .Seal()
                            .Seal()
                        .Seal()
                        .Build();
                }

                TVector<std::pair<TExprNode::TPtr, TExprNode::TPtr>> aggs;
                TNodeMap<ui32> aggId;
                VisitExpr(projectionLambda->TailPtr(), [&](const TExprNode::TPtr& node) {
                    if (node->IsCallable("PgAgg") || node->IsCallable("PgAggAll")) {
                        aggId[node.Get()] = aggs.size();
                        aggs.push_back({ node, projectionLambda->Head().HeadPtr() });
                    }

                    return true;
                });

                if (having) {
                    auto havingLambda = having->Tail().TailPtr();
                    VisitExpr(having->Tail().TailPtr(), [&](const TExprNode::TPtr& node) {
                        if (node->IsCallable("PgAgg") || node->IsCallable("PgAggAll")) {
                            aggId[node.Get()] = aggs.size();
                            aggs.push_back({ node, havingLambda->Head().HeadPtr() });
                        }

                        return true;
                    });
                }

                if (!aggs.empty() || groupBy) {
                    auto listTypeNode = ctx.Builder(node->Pos())
                        .Callable("TypeOf")
                            .Add(0, list)
                        .Seal()
                        .Build();

                    auto exportsPtr = optCtx.Types->Modules->GetModule("/lib/yql/aggregate.yql");
                    YQL_ENSURE(exportsPtr);

                    TNodeOnNodeOwnedMap deepClones;
                    TExprNode::TListType payloadItems;
                    for (ui32 i = 0; i < aggs.size(); ++i) {
                        const auto& exports = exportsPtr->Symbols();
                        auto func = aggs[i].first->Head().Content();
                        if (func == "count" && aggs[i].first->ChildrenSize() == 1) {
                            func = "count_all";
                        }

                        TString factory = TString(func) + "_traits_factory";
                        const auto ex = exports.find(factory);
                        YQL_ENSURE(exports.cend() != ex);
                        auto lambda = ctx.DeepCopy(*ex->second, exportsPtr->ExprCtx(), deepClones, true, false);
                        auto arg = ctx.NewArgument(node->Pos(), "row");
                        auto arguments = ctx.NewArguments(node->Pos(), { arg });
                        auto extractor = ctx.NewLambda(node->Pos(), std::move(arguments),
                            ctx.ReplaceNode(aggs[i].first->TailPtr(), *aggs[i].second, arg));

                        auto traits = ctx.ReplaceNodes(lambda->TailPtr(), {
                            {lambda->Head().Child(0), listTypeNode},
                            {lambda->Head().Child(1), extractor}
                        });

                        ctx.Step.Repeat(TExprStep::ExpandApplyForLambdas);
                        auto status = ExpandApply(traits, traits, ctx);
                        if (status == IGraphTransformer::TStatus::Error) {
                            return {};
                        }

                        payloadItems.push_back(ctx.Builder(node->Pos())
                            .List()
                                .Atom(0, "_yql_agg_" + ToString(i))
                                .Add(1, traits)
                            .Seal()
                            .Build());
                    }

                    auto payloadsNode = ctx.NewList(node->Pos(), std::move(payloadItems));
                    TExprNode::TListType keysItems;
                    if (groupBy) {
                        for (const auto& group : groupBy->Tail().Children()) {
                            const auto& lambda = group->Tail();
                            YQL_ENSURE(lambda.IsLambda());
                            YQL_ENSURE(lambda.Tail().IsCallable("Member"));
                            keysItems.push_back(lambda.Tail().TailPtr());
                        }
                    }

                    auto keys = ctx.NewList(node->Pos(), std::move(keysItems));

                    list = ctx.Builder(node->Pos())
                        .Callable("Aggregate")
                            .Add(0, list)
                            .Add(1, keys)
                            .Add(2, payloadsNode)
                            .List(3) // options
                            .Seal()
                        .Seal()
                        .Build();

                    auto rewriteAggs = [&](auto& lambda) {
                        auto status = OptimizeExpr(lambda, lambda, [&](const TExprNode::TPtr& node, TExprContext& ctx) -> TExprNode::TPtr {
                            auto it = aggId.find(node.Get());
                            if (it != aggId.end()) {
                                auto ret = ctx.Builder(node->Pos())
                                    .Callable("Member")
                                        .Add(0, lambda->Head().HeadPtr())
                                        .Atom(1, "_yql_agg_" + ToString(it->second))
                                    .Seal()
                                    .Build();
                                if (node->Head().Content() == "count") {
                                    ret = ctx.Builder(node->Pos())
                                        .Callable("SafeCast")
                                            .Add(0, ret)
                                            .Atom(1, "Int64")
                                        .Seal()
                                        .Build();
                                }

                                return ret;
                            }

                            return node;
                        }, ctx, TOptimizeExprSettings(optCtx.Types));

                        return status.Level != IGraphTransformer::TStatus::Error;
                    };

                    if (!rewriteAggs(projectionLambda)) {
                        return {};
                    }

                    if (having) {
                        auto havingLambda = having->Tail().TailPtr();
                        if (!rewriteAggs(havingLambda)) {
                            return {};
                        }

                        list = ctx.Builder(node->Pos())
                            .Callable("Filter")
                                .Add(0, list)
                                .Lambda(1)
                                    .Param("row")
                                    .Apply(havingLambda)
                                        .With(0, "row")
                                    .Seal()
                                .Seal()
                            .Seal()
                            .Build();
                    }
                }

                TVector<std::pair<TExprNode::TPtr, TExprNode::TPtr>> winFuncs;
                TMap<ui32, TVector<ui32>> window2funcs;
                TNodeMap<ui32> winFuncsId;
                bool hasAggsOverWindow = false;
                VisitExpr(projectionLambda->TailPtr(), [&](const TExprNode::TPtr& node) {
                    if (node->IsCallable("PgWindowCall") || node->IsCallable("PgAggWindowCall")) {
                        hasAggsOverWindow = hasAggsOverWindow || node->IsCallable("PgAggWindowCall");
                        YQL_ENSURE(window);
                        ui32 windowIndex;
                        if (node->Child(1)->IsCallable("PgAnonWindow")) {
                            windowIndex = FromString<ui32>(node->Child(1)->Head().Content());
                        } else {
                            auto name = node->Child(1)->Content();
                            bool found = false;
                            for (ui32 index = 0; index < window->Tail().ChildrenSize(); ++index) {
                                if (window->Tail().Child(index)->Head().Content() == name) {
                                    windowIndex = index;
                                    found = true;
                                    break;
                                }
                            }

                            YQL_ENSURE(found);
                        }

                        window2funcs[windowIndex].push_back(winFuncs.size());
                        winFuncsId[node.Get()] = winFuncs.size();
                        winFuncs.push_back({ node, projectionLambda->Head().HeadPtr() });
                    }

                    return true;
                });

                if (!winFuncs.empty()) {
                    auto listTypeNode = ctx.Builder(node->Pos())
                        .Callable("TypeOf")
                            .Add(0, list)
                        .Seal()
                        .Build();

                    TNodeOnNodeOwnedMap deepClones;
                    const TExportTable* exportsPtr = nullptr;
                    if (hasAggsOverWindow) {
                        exportsPtr = optCtx.Types->Modules->GetModule("/lib/yql/window.yql");
                        YQL_ENSURE(exportsPtr);
                    }

                    for (const auto& x : window2funcs) {
                        auto win = window->Tail().Child(x.first);
                        const auto& frameSettings = win->Tail();

                        TExprNode::TListType args;
                        // default frame
                        auto begin = ctx.NewCallable(node->Pos(), "Void", {});
                        auto end = win->Child(3)->ChildrenSize() > 0 ?
                            ctx.NewCallable(node->Pos(), "Int32", { ctx.NewAtom(node->Pos(), "0") }) :
                            ctx.NewCallable(node->Pos(), "Void", {});
                        if (HasSetting(frameSettings, "type")) {
                            const auto& from = GetSetting(frameSettings, "from");
                            const auto& fromValue = GetSetting(frameSettings, "from_value");

                            auto fromName = from->Tail().Content();
                            if (fromName == "up") {
                                begin = ctx.NewCallable(node->Pos(), "Void", {});
                            } else if (fromName == "p") {
                                auto val = FromString<i32>(fromValue->Tail().Head().Content());
                                begin = ctx.NewCallable(node->Pos(), "Int32", { ctx.NewAtom(node->Pos(), ToString(-val)) });
                            } else if (fromName == "c") {
                                begin = ctx.NewCallable(node->Pos(), "Int32", { ctx.NewAtom(node->Pos(), "0") });
                            } else {
                                YQL_ENSURE(fromName == "f");
                                auto val = FromString<i32>(fromValue->Tail().Head().Content());
                                begin = ctx.NewCallable(node->Pos(), "Int32", { ctx.NewAtom(node->Pos(), ToString(val)) });
                            }

                            const auto& to = GetSetting(frameSettings, "to");
                            const auto& toValue = GetSetting(frameSettings, "to_value");

                            auto toName = to->Tail().Content();
                            if (toName == "p") {
                                auto val = FromString<i32>(toValue->Tail().Head().Content());
                                end = ctx.NewCallable(node->Pos(), "Int32", { ctx.NewAtom(node->Pos(), ToString(-val)) });
                            } else if (toName == "c") {
                                end = ctx.NewCallable(node->Pos(), "Int32", { ctx.NewAtom(node->Pos(), "0") });
                            } else if (toName == "f") {
                                auto val = FromString<i32>(toValue->Tail().Head().Content());
                                end = ctx.NewCallable(node->Pos(), "Int32", { ctx.NewAtom(node->Pos(), ToString(val)) });
                            } else {
                                YQL_ENSURE(toName == "uf");
                                end = ctx.NewCallable(node->Pos(), "Void", {});
                            }
                        }

                        args.push_back(ctx.Builder(node->Pos())
                            .List()
                                .List(0)
                                    .Atom(0, "begin")
                                    .Add(1, begin)
                                .Seal()
                                .List(1)
                                    .Atom(0, "end")
                                    .Add(1, end)
                                .Seal()
                            .Seal()
                            .Build());

                        for (const auto& index : x.second) {
                            auto p = winFuncs[index];
                            auto name = p.first->Head().Content();
                            bool isAgg = p.first->IsCallable("PgAggWindowCall");
                            TExprNode::TPtr value;
                            if (isAgg) {
                                const auto& exports = exportsPtr->Symbols();
                                if (name == "count" && p.first->ChildrenSize() == 2) {
                                    name = "count_all";
                                }

                                TString factory = TString(name) + "_traits_factory";
                                const auto ex = exports.find(factory);
                                YQL_ENSURE(exports.cend() != ex);
                                auto lambda = ctx.DeepCopy(*ex->second, exportsPtr->ExprCtx(), deepClones, true, false);
                                auto arg = ctx.NewArgument(node->Pos(), "row");
                                auto arguments = ctx.NewArguments(node->Pos(), { arg });
                                auto extractor = ctx.NewLambda(node->Pos(), std::move(arguments),
                                    ctx.ReplaceNode(p.first->TailPtr(), *p.second, arg));

                                auto traits = ctx.ReplaceNodes(lambda->TailPtr(), {
                                    {lambda->Head().Child(0), listTypeNode},
                                    {lambda->Head().Child(1), extractor}
                                    });

                                ctx.Step.Repeat(TExprStep::ExpandApplyForLambdas);
                                auto status = ExpandApply(traits, traits, ctx);
                                if (status == IGraphTransformer::TStatus::Error) {
                                    return {};
                                }

                                value = traits;
                            } else {
                                if (name == "row_number") {
                                    value = ctx.Builder(node->Pos())
                                        .Callable("RowNumber")
                                            .Callable(0, "TypeOf")
                                                .Add(0, list)
                                            .Seal()
                                        .Seal()
                                        .Build();
                                } else if (name == "lead" || name == "lag") {
                                    auto arg = ctx.NewArgument(node->Pos(), "row");
                                    auto arguments = ctx.NewArguments(node->Pos(), { arg });
                                    auto extractor = ctx.NewLambda(node->Pos(), std::move(arguments),
                                        ctx.ReplaceNode(p.first->TailPtr(), *p.second, arg));

                                    value = ctx.Builder(node->Pos())
                                        .Callable(name == "lead" ? "Lead" : "Lag")
                                            .Callable(0, "TypeOf")
                                                .Add(0, list)
                                            .Seal()
                                            .Add(1, extractor)
                                        .Seal()
                                        .Build();
                                } else {
                                    ythrow yexception() << "Not supported function: " << name;
                                }
                            }

                            args.push_back(ctx.Builder(node->Pos())
                                .List()
                                    .Atom(0, "_yql_win_" + ToString(index))
                                    .Add(1, value)
                                .Seal()
                                .Build());
                        }

                        auto winOnRows = ctx.NewCallable(node->Pos(), "WinOnRows", std::move(args));

                        auto frames = ctx.Builder(node->Pos())
                            .List()
                                .Add(0, winOnRows)
                            .Seal()
                            .Build();

                        TExprNode::TListType keys;
                        for (auto p : win->Child(2)->Children()) {
                            YQL_ENSURE(p->IsCallable("PgGroup"));
                            const auto& member = p->Tail().Tail();
                            YQL_ENSURE(member.IsCallable("Member"));
                            keys.push_back(member.TailPtr());
                        }

                        auto keysNode = ctx.NewList(node->Pos(), std::move(keys));
                        auto sortNode = ctx.NewCallable(node->Pos(), "Void", {});
                        if (win->Child(3)->ChildrenSize() > 0) {
                            if (win->Child(3)->ChildrenSize() == 1) {
                                sortNode = ctx.Builder(node->Pos())
                                    .Callable("SortTraits")
                                        .Callable(0, "TypeOf")
                                            .Add(0, list)
                                        .Seal()
                                        .Callable(1, "Bool")
                                            .Atom(0, win->Child(3)->Head().Tail().Content() == "asc" ? "true" : "false")
                                        .Seal()
                                        .Lambda(2)
                                            .Param("row")
                                            .Apply(win->Child(3)->Head().ChildPtr(1))
                                                .With(0, "row")
                                            .Seal()
                                        .Seal()
                                    .Seal()
                                    .Build();
                            } else {
                                sortNode = ctx.Builder(node->Pos())
                                    .Callable("SortTraits")
                                        .Callable(0, "TypeOf")
                                            .Add(0, list)
                                        .Seal()
                                        .List(1)
                                            .Do([&](TExprNodeBuilder& parent) -> TExprNodeBuilder& {
                                                for (ui32 i = 0; i < win->Child(3)->ChildrenSize(); ++i) {
                                                    parent.Callable(i, "Bool")
                                                        .Atom(0, win->Child(3)->Child(i)->Tail().Content() == "asc" ? "true" : "false")
                                                        .Seal();
                                                }
                                                return parent;
                                            })
                                        .Seal()
                                        .Lambda(2)
                                            .Param("row")
                                            .List()
                                                .Do([&](TExprNodeBuilder& parent) -> TExprNodeBuilder& {
                                                    for (ui32 i = 0; i < win->Child(3)->ChildrenSize(); ++i) {
                                                        parent.Apply(i, win->Child(3)->Child(i)->ChildPtr(1))
                                                            .With(0, "row")
                                                            .Seal();
                                                    }

                                                    return parent;
                                                })
                                            .Seal()
                                        .Seal()
                                    .Seal()
                                    .Build();
                            }
                        }

                        list = ctx.Builder(node->Pos())
                            .Callable("CalcOverWindow")
                                .Add(0, list)
                                .Add(1, keysNode)
                                .Add(2, sortNode)
                                .Add(3, frames)
                            .Seal()
                            .Build();
                    }

                    auto status = OptimizeExpr(projectionLambda, projectionLambda, [&](const TExprNode::TPtr& node, TExprContext& ctx) -> TExprNode::TPtr {
                        auto it = winFuncsId.find(node.Get());
                        if (it != winFuncsId.end()) {
                            auto ret = ctx.Builder(node->Pos())
                                .Callable("Member")
                                    .Add(0, projectionLambda->Head().HeadPtr())
                                    .Atom(1, "_yql_win_" + ToString(it->second))
                                .Seal()
                                .Build();

                            if (node->Head().Content() == "row_number" || node->Head().Content() == "count") {
                                ret = ctx.Builder(node->Pos())
                                    .Callable("SafeCast")
                                        .Add(0, ret)
                                        .Atom(1, "Int64")
                                    .Seal()
                                    .Build();
                            }

                            return ret;
                        }

                        return node;
                    }, ctx, TOptimizeExprSettings(optCtx.Types));

                    if (status.Level == IGraphTransformer::TStatus::Error) {
                        return nullptr;
                    }
                }

                list = ctx.Builder(node->Pos())
                    .Callable("Map")
                        .Add(0, list)
                        .Add(1, projectionLambda)
                    .Seal()
                    .Build();
            }

            setItemNodes.push_back(list);
        }

        TExprNode::TPtr list;
        if (setItemNodes.size() == 1) {
            list = setItemNodes.front();
        } else {
            list = ExpandPositionalUnionAll(*node, columnOrders, setItemNodes, ctx, optCtx);
        }

        auto sort = GetSetting(node->Head(), "sort");
        if (sort && sort->Tail().ChildrenSize() > 0) {
            const auto& keys = sort->Tail();
            auto argNode = ctx.NewArgument(node->Pos(), "row");
            auto argsNode = ctx.NewArguments(node->Pos(), { argNode });

            TExprNode::TListType dirItems;
            TExprNode::TListType rootItems;
            for (const auto& key : keys.Children()) {
                dirItems.push_back(ctx.Builder(node->Pos())
                    .Callable("Bool")
                        .Atom(0, key->Tail().Content() == "asc" ? "true" : "false")
                    .Seal()
                    .Build());

                auto keyLambda = key->ChildPtr(1);
                rootItems.push_back(ctx.ReplaceNode(keyLambda->TailPtr(), keyLambda->Head().Head(), argNode));
            }

            auto root = ctx.NewList(node->Pos(), std::move(rootItems));
            auto dir = ctx.NewList(node->Pos(), std::move(dirItems));
            auto lambda = ctx.NewLambda(node->Pos(), std::move(argsNode), std::move(root));

            list = ctx.Builder(node->Pos())
                .Callable("Sort")
                    .Add(0, list)
                    .Add(1, dir)
                    .Add(2, lambda)
                .Seal()
                .Build();
        }

        auto limit = GetSetting(node->Head(), "limit");
        auto offset = GetSetting(node->Head(), "offset");

        if (offset) {
            list = ctx.Builder(node->Pos())
                .Callable("Skip")
                    .Add(0, list)
                    .Callable(1, "Unwrap")
                        .Callable(0, "SafeCast")
                            .Callable(0, "Coalesce")
                                .Add(0, offset->ChildPtr(1))
                                .Callable(1, "Int64")
                                    .Atom(0, "0")
                                .Seal()
                            .Seal()
                            .Atom(1, "Uint64")
                        .Seal()
                        .Callable(1, "String")
                            .Atom(0, "Negative offset")
                        .Seal()
                    .Seal()
                .Seal()
                .Build();
        }

        if (limit) {
            list = ctx.Builder(node->Pos())
                .Callable("Take")
                    .Add(0, list)
                    .Callable(1, "Unwrap")
                        .Callable(0, "SafeCast")
                            .Callable(0, "Coalesce")
                                .Add(0, limit->ChildPtr(1))
                                .Callable(1, "Int64")
                                    .Atom(0, "9223372036854775807") // 2**63-1
                                .Seal()
                            .Seal()
                            .Atom(1, "Uint64")
                        .Seal()
                        .Callable(1, "String")
                            .Atom(0, "Negative limit")
                        .Seal()
                    .Seal()
                .Seal()
                .Build();
        }

        return ctx.Builder(node->Pos())
            .Callable("AssumeColumnOrder")
                .Add(0, list)
                .Add(1, columns)
            .Seal()
            .Build();
    };

    map["SqlColumnOrType"] = map["SqlPlainColumnOrType"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        YQL_CLOG(DEBUG, Core) << "Decay of never inspected " << node->Content();
        return ctx.NewCallable(node->Pos(), "Error", { ExpandType(node->Pos(), *node->GetTypeAnn(), ctx) });
    };

    map["SqlColumnFromType"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& /*optCtx*/) {
        YQL_CLOG(DEBUG, Core) << "Decay of " << node->Content();
        return ctx.NewCallable(node->Pos(), "Member", { node->HeadPtr(), node->ChildPtr(1) });
    };

    // will be applied to any callable after all above
    map[""] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) {
        YQL_ENSURE(node->IsCallable());

        if (AnyOf(node->ChildrenList(), [](const auto& child) { return child->IsCallable("AssumeColumnOrder"); })) {
            auto type = node->GetTypeAnn();
            if (type->GetKind() == ETypeAnnotationKind::World) {
                // stop on world
                return node;
            }

            // push over sequence-of-structs or tuple(world, sequence-of-structs)
            if (type->GetKind() == ETypeAnnotationKind::Tuple) {
                auto tupleType = type->Cast<TTupleExprType>();
                if (tupleType->GetSize() == 2 && tupleType->GetItems()[0]->GetKind() == ETypeAnnotationKind::World) {
                    type = tupleType->GetItems()[1];
                }
            }

            if (type->GetKind() != ETypeAnnotationKind::Struct) {
                type = GetItemType(*type);
            }


            auto newChildren = node->ChildrenList();
            for (auto& child : newChildren) {
                if (child->IsCallable("AssumeColumnOrder")) {
                    child = child->HeadPtr();
                }
            }

            auto result = ctx.ChangeChildren(*node, std::move(newChildren));
            if (type && type->GetKind() == ETypeAnnotationKind::Struct) {
                YQL_CLOG(DEBUG, Core) << "Pull AssumeColumnOrder over " << node->Content();
                result = KeepColumnOrder(result, *node, ctx, *optCtx.Types);
            } else {
                YQL_CLOG(DEBUG, Core) << "Drop AssumeColumnOrder as input of " << node->Content();
            }

            return result;
        }

        return node;
    };
}

} // namespace NYql
