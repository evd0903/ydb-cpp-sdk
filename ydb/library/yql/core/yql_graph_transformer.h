#pragma once
#include <ydb/library/yql/ast/yql_expr.h>
#include <ydb/library/yql/utils/yql_panic.h>
#include <ydb/library/yql/core/issue/yql_issue.h>

#include <library/cpp/threading/future/future.h>

#include <util/generic/hash.h>
#include <util/datetime/base.h>

#include <functional>

namespace NYql {

class IGraphTransformer {
public:
    struct TStatus {
#define YQL_GT_STATUS_MAP(xx) \
    xx(Ok, 0) \
    xx(Repeat, 1) \
    xx(Async, 2) \
    xx(Error, 3)

        enum ELevel {
            YQL_GT_STATUS_MAP(ENUM_VALUE_GEN)
        };

        union {
            ui32 Raw;
            struct {
                ui32 Level : 4;
                ui32 HasRestart : 1;
                ui32 Padding : 27;
            };
        };

        bool operator== (const TStatus& other) const {
            return Raw == other.Raw;
        }

        bool operator!= (const TStatus& other) const {
            return Raw != other.Raw;
        }

        bool operator== (ELevel other) const {
            return Level == other;
        }

        bool operator!= (ELevel other) const {
            return Level != other;
        }

        TStatus(ELevel level, bool hasRestart = false)
            : Level(level)
            , HasRestart(hasRestart)
            , Padding(0)
        {}

        [[nodiscard]]
        TStatus Combine(TStatus other) const {
            const bool hasRestart = HasRestart || other.HasRestart;
            return TStatus((TStatus::ELevel)Max(Level, other.Level), hasRestart);
        }

        void Out(IOutputStream &out) const {
            out << (TStatus::ELevel)Level;
            if (HasRestart) {
                out << ", with restart";
            }
        }
    };

    struct TStatistics {
        TDuration TransformDuration;
        TDuration WaitDuration;
        i32 NewExprNodes;
        i32 NewTypeNodes;
        i32 NewConstraintNodes;
        ui32 Repeats;
        ui32 Restarts;

        TVector<std::pair<TString, TStatistics>> Stages;

        TStatistics()
            : TransformDuration(TDuration::Zero())
            , WaitDuration(TDuration::Zero())
            , NewExprNodes(0)
            , NewTypeNodes(0)
            , NewConstraintNodes(0)
            , Repeats(0)
            , Restarts(0)
            , Stages() {}

        static TStatistics NotPresent() { return TStatistics(); }
        static TStatistics Zero() { return TStatistics(); }
    };

    virtual ~IGraphTransformer() {}

    virtual TStatus Transform(TExprNode::TPtr input, TExprNode::TPtr& output, TExprContext& ctx) = 0;
    virtual NThreading::TFuture<void> GetAsyncFuture(const TExprNode& input) = 0;
    virtual TStatus ApplyAsyncChanges(TExprNode::TPtr input, TExprNode::TPtr& output, TExprContext& ctx) = 0;
    virtual void Rewind() {}

    virtual TStatistics GetStatistics() const { return TStatistics::NotPresent(); }
};

class TGraphTransformerBase : public IGraphTransformer {
private:
    class TTransformScope {
    public:
        TTransformScope(TStatistics& statistics, const TExprContext* exprCtx)
            : Statistics(statistics)
            , ExprCtx(exprCtx)
            , TransformStart(TInstant::Now())
            , ExprNodesSize(exprCtx ? exprCtx->ExprNodes.size() : 0)
            , TypeNodesSize(exprCtx ? exprCtx->TypeNodes.size() : 0)
            , ConstraintNodesSize(exprCtx ? exprCtx->ConstraintNodes.size() : 0)
        {
        }

        ~TTransformScope() {
            Statistics.TransformDuration += TInstant::Now() - TransformStart;
            if (ExprCtx) {
                Statistics.NewExprNodes += ExprCtx->ExprNodes.size() - ExprNodesSize;
                Statistics.NewTypeNodes += ExprCtx->TypeNodes.size() - TypeNodesSize;
                Statistics.NewConstraintNodes += ExprCtx->ConstraintNodes.size() - ConstraintNodesSize;
            }
        }

        TStatus HandleStatus(const TStatus& status) {
            if (status == TStatus::Repeat) {
                Statistics.Repeats++;
            }

            if (status.HasRestart) {
                Statistics.Restarts++;
            }

            return status;
        }

    private:
        TStatistics& Statistics;
        const TExprContext* ExprCtx;
        TInstant TransformStart;
        i64 ExprNodesSize;
        i64 TypeNodesSize;
        i64 ConstraintNodesSize;
    };

public:
    TGraphTransformerBase()
        : Statistics(TStatistics::Zero())
        , AsyncStart() {}

    TStatus Transform(TExprNode::TPtr input, TExprNode::TPtr& output, TExprContext& ctx) final {
        TTransformScope scope(Statistics, &ctx);

        return scope.HandleStatus(DoTransform(input, output, ctx));
    }

    NThreading::TFuture<void> GetAsyncFuture(const TExprNode& input) final {
        TTransformScope scope(Statistics, nullptr);
        AsyncStart = TInstant::Now();

        return DoGetAsyncFuture(input);
    }

    TStatus ApplyAsyncChanges(TExprNode::TPtr input, TExprNode::TPtr& output, TExprContext& ctx) final {
        TTransformScope scope(Statistics, &ctx);
        Statistics.WaitDuration += TInstant::Now() - AsyncStart;

        return scope.HandleStatus(DoApplyAsyncChanges(input, output, ctx));
    }

    virtual TStatistics GetStatistics() const override { return Statistics; }

public:
    virtual TStatus DoTransform(TExprNode::TPtr input, TExprNode::TPtr& output, TExprContext& ctx) = 0;
    virtual NThreading::TFuture<void> DoGetAsyncFuture(const TExprNode& input) = 0;
    virtual TStatus DoApplyAsyncChanges(TExprNode::TPtr input, TExprNode::TPtr& output, TExprContext& ctx) = 0;

protected:
    mutable TStatistics Statistics;

private:
    TInstant AsyncStart;
};

struct TTransformStage {
    TString Name;
    EYqlIssueCode IssueCode;
    TString IssueMessage;

    TTransformStage(const TAutoPtr<IGraphTransformer>& transformer, const TString& name, EYqlIssueCode issueCode, const TString& issueMessage = {})
        : Name(name)
        , IssueCode(issueCode)
        , IssueMessage(issueMessage)
        , RawTransformer(transformer.Get())
        , Transformer(transformer)
    {}

    TTransformStage(IGraphTransformer& transformer, const TString& name, EYqlIssueCode issueCode, const TString& issueMessage = {})
        : Name(name)
        , IssueCode(issueCode)
        , IssueMessage(issueMessage)
        , RawTransformer(&transformer)
    {}

    IGraphTransformer& GetTransformer() const
    {
        return *RawTransformer;
    }
private:
    IGraphTransformer* const RawTransformer;
    const TAutoPtr<IGraphTransformer> Transformer;
};

TAutoPtr<IGraphTransformer> CreateCompositeGraphTransformer(const TVector<TTransformStage>& stages, bool useIssueScopes);
TAutoPtr<IGraphTransformer> CreateCompositeGraphTransformerWithNoArgChecks(const TVector<TTransformStage>& stages, bool useIssueScopes);

TAutoPtr<IGraphTransformer> CreateChoiceGraphTransformer( 
    const std::function<bool(const TExprNode::TPtr& input, TExprContext& ctx)>& condition, 
    const TTransformStage& left, 
    const TTransformStage& right); 
 
IGraphTransformer::TStatus SyncTransform(IGraphTransformer& transformer, TExprNode::TPtr& root, TExprContext& ctx);
IGraphTransformer::TStatus InstantTransform(IGraphTransformer& transformer, TExprNode::TPtr& root, TExprContext& ctx, bool breakOnRestart = false);

NThreading::TFuture<IGraphTransformer::TStatus> AsyncTransform(IGraphTransformer& transformer, TExprNode::TPtr& root, TExprContext& ctx, bool applyAsyncChanges);

void AsyncTransform(IGraphTransformer& transformer, TExprNode::TPtr& root, TExprContext& ctx, bool applyAsyncChanges,
                    std::function<void(const IGraphTransformer::TStatus&)> asyncCallback);

class TSyncTransformerBase : public TGraphTransformerBase {
public:
    NThreading::TFuture<void> DoGetAsyncFuture(const TExprNode& input) final {
        Y_UNUSED(input);
        YQL_ENSURE(false, "Not supported");
    }

    TStatus DoApplyAsyncChanges(TExprNode::TPtr input, TExprNode::TPtr& output, TExprContext& ctx) final {
        Y_UNUSED(input);
        Y_UNUSED(output);
        Y_UNUSED(ctx);
        YQL_ENSURE(false, "Not supported");
    }
};

class TNullTransformer : public TSyncTransformerBase {
public:
    TStatus DoTransform(TExprNode::TPtr input, TExprNode::TPtr& output, TExprContext& ctx) final {
        output = input;
        Y_UNUSED(ctx);

        return IGraphTransformer::TStatus::Ok;
    }
};

template <typename TFunctor>
class TFunctorTransformer : public TSyncTransformerBase {
public:
    TFunctorTransformer(TFunctor functor)
        : Functor(std::move(functor)) {}

    TStatus DoTransform(TExprNode::TPtr input, TExprNode::TPtr& output, TExprContext& ctx) final {
        TStatus status = Functor(input, output, ctx);
        YQL_ENSURE(status.Level != IGraphTransformer::TStatus::Async);

        return status;
    }

private:
    TFunctor Functor;
};

template <typename TFunctor>
THolder<IGraphTransformer> CreateFunctorTransformer(TFunctor functor) {
    return MakeHolder<TFunctorTransformer<TFunctor>>(std::move(functor));
}

typedef std::function<IGraphTransformer::TStatus(const TExprNode::TPtr&, TExprNode::TPtr&, TExprContext&)> TAsyncTransformCallback;
typedef NThreading::TFuture<TAsyncTransformCallback> TAsyncTransformCallbackFuture;

template <typename TDerived>
class TAsyncCallbackTransformer : public TGraphTransformerBase {
public:
    // CallbackTransform should return std::pair<TStatus, TAsyncTransformCallbackFuture>
    TStatus DoTransform(TExprNode::TPtr input, TExprNode::TPtr& output, TExprContext& ctx) final {
        auto pair = static_cast<TDerived*>(this)->CallbackTransform(input, output, ctx);
        if (pair.first == TStatus::Async) {
            YQL_ENSURE(Callbacks.emplace(input.Get(), pair.second).second);
        }

        return pair.first;
    }

    NThreading::TFuture<void> DoGetAsyncFuture(const TExprNode& input) final {
        const auto it = Callbacks.find(&input);
        YQL_ENSURE(it != Callbacks.cend());
        return it->second.IgnoreResult();
    }

    TStatus DoApplyAsyncChanges(TExprNode::TPtr input, TExprNode::TPtr& output, TExprContext& ctx) final {
        const auto it = Callbacks.find(input.Get());
        YQL_ENSURE(it != Callbacks.cend());
        auto& future = it->second;
        YQL_ENSURE(future.HasValue());
        const auto status = future.GetValue()(input, output, ctx);
        Callbacks.erase(it);
        return status;
    }

    void Rewind() override {
        Callbacks.clear();
    }

private:
    TNodeMap<TAsyncTransformCallbackFuture> Callbacks;
};

template <typename TFuture, typename TCallback>
std::pair<IGraphTransformer::TStatus, TAsyncTransformCallbackFuture>
WrapFutureCallback(const TFuture& future, const TCallback& callback, const TString& message = "") {
    return std::make_pair(IGraphTransformer::TStatus::Async, future.Apply(
        [callback, message](const TFuture& completedFuture) {
            return TAsyncTransformCallback([completedFuture, callback, message](const TExprNode::TPtr& input,
                TExprNode::TPtr& output, TExprContext& ctx)
                {
                    output = input;
                    const auto& res = completedFuture.GetValue();

                    TIssueScopeGuard issueScope(ctx.IssueManager, [&]() {
                        return MakeIntrusive<TIssue>(
                            ctx.GetPosition(input->Pos()),
                            message.empty()
                                ? TStringBuilder() << "Execution of node: " << input->Content()
                                : message);
                    });
                    res.ReportIssues(ctx.IssueManager);

                    if (!res.Success()) {
                        input->SetState(TExprNode::EState::Error);
                        return IGraphTransformer::TStatus(IGraphTransformer::TStatus::Error);
                    }
                    else {
                        return callback(res, input, output, ctx);
                    }
                });
        }));
}

template <typename TFuture, typename TResultExtractor>
std::pair<IGraphTransformer::TStatus, TAsyncTransformCallbackFuture>
WrapFuture(const TFuture& future, const TResultExtractor& extractor, const TString& message = "") {
    return WrapFutureCallback(future, [extractor](const NThreading::TFutureType<TFuture>& res, const TExprNode::TPtr& input, TExprNode::TPtr& /*output*/, TExprContext& ctx) {
        input->SetState(TExprNode::EState::ExecutionComplete);
        input->SetResult(extractor(res, input, ctx));
        return IGraphTransformer::TStatus(IGraphTransformer::TStatus::Ok);
    }, message);
}

template <typename TFuture, typename TResultExtractor>
std::pair<IGraphTransformer::TStatus, TAsyncTransformCallbackFuture>
WrapModifyFuture(const TFuture& future, const TResultExtractor& extractor, const TString& message = "") {
    return WrapFutureCallback(future, [extractor](const NThreading::TFutureType<TFuture>& res, const TExprNode::TPtr& input, TExprNode::TPtr& output, TExprContext& ctx) {
        TExprNode::TPtr resultNode = extractor(res, input, output, ctx);
        input->SetState(TExprNode::EState::ExecutionComplete);
        output->SetResult(std::move(resultNode));
        if (input != output) {
            return IGraphTransformer::TStatus(IGraphTransformer::TStatus::Repeat, true);
        }
        return IGraphTransformer::TStatus(IGraphTransformer::TStatus::Ok);
    }, message);
}

inline std::pair<IGraphTransformer::TStatus, TAsyncTransformCallbackFuture> SyncStatus(IGraphTransformer::TStatus status) {
    return std::make_pair(status, TAsyncTransformCallbackFuture());
}

inline std::pair<IGraphTransformer::TStatus, TAsyncTransformCallbackFuture> SyncError() {
    return SyncStatus(IGraphTransformer::TStatus::Error);
}

inline std::pair<IGraphTransformer::TStatus, TAsyncTransformCallbackFuture> SyncOk() {
    return SyncStatus(IGraphTransformer::TStatus::Ok);
}

inline std::pair<IGraphTransformer::TStatus, TAsyncTransformCallbackFuture> SyncRepeat() {
    return SyncStatus(IGraphTransformer::TStatus::Repeat);
}

typedef std::unordered_map<TExprNode::TPtr, ui64, TExprNode::TPtrHash> TSyncMap;
}

template<>
inline void Out<NYql::IGraphTransformer::TStatus>(
    IOutputStream &out, const NYql::IGraphTransformer::TStatus& status)
{
    status.Out(out);
}
