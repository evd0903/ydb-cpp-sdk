#include "yql_s3_write_actor.h"
#include "yql_s3_actors_util.h"

#include <ydb/core/protos/services.pb.h>

#include <ydb/library/yql/providers/common/http_gateway/yql_http_default_retry_policy.h>
#include <ydb/library/yql/providers/common/provider/yql_provider_names.h>
#include <ydb/library/yql/providers/s3/compressors/factory.h>
#include <ydb/library/yql/providers/s3/common/util.h>
#include <ydb/library/yql/utils/yql_panic.h>

#include <library/cpp/actors/core/actor_bootstrapped.h>
#include <library/cpp/actors/core/events.h>
#include <library/cpp/actors/core/event_local.h>
#include <library/cpp/actors/core/hfunc.h>
#include <library/cpp/actors/core/log.h>
#include <library/cpp/actors/http/http.h>
#include <library/cpp/string_utils/base64/base64.h>
#include <library/cpp/string_utils/quote/quote.h>

#include <util/generic/size_literals.h>

#include <queue>

#ifdef THROW
#undef THROW
#endif
#include <library/cpp/xml/document/xml-document.h>


#define LOG_E(name, stream) \
    LOG_ERROR_S(*TlsActivationContext, NKikimrServices::KQP_COMPUTE, name << ": " << this->SelfId() << ", TxId: " << TxId << ". " << stream)
#define LOG_W(name, stream) \
    LOG_WARN_S(*TlsActivationContext, NKikimrServices::KQP_COMPUTE, name << ": " << this->SelfId() << ", TxId: " << TxId << ". " << stream)
#define LOG_I(name, stream) \
    LOG_INFO_S(*TlsActivationContext, NKikimrServices::KQP_COMPUTE, name << ": " << this->SelfId() << ", TxId: " << TxId << ". " << stream)
#define LOG_D(name, stream) \
    LOG_DEBUG_S(*TlsActivationContext, NKikimrServices::KQP_COMPUTE, name << ": " << this->SelfId() << ", TxId: " << TxId << ". " << stream)
#define LOG_T(name, stream) \
    LOG_TRACE_S(*TlsActivationContext, NKikimrServices::KQP_COMPUTE, name << ": " << this->SelfId() << ", TxId: " << TxId << ". " << stream)

namespace NYql::NDq {

using namespace NActors;

namespace {

struct TEvPrivate {
    // Event ids
    enum EEv : ui32 {
        EvBegin = EventSpaceBegin(TEvents::ES_PRIVATE),

        EvUploadError = EvBegin,
        EvUploadStarted,
        EvUploadPartFinished,
        EvUploadFinished,

        EvEnd
    };

    static_assert(EvEnd < EventSpaceEnd(TEvents::ES_PRIVATE), "expect EvEnd < EventSpaceEnd(TEvents::ES_PRIVATE)");

    // Events
    struct TEvUploadFinished : public TEventLocal<TEvUploadFinished, EvUploadFinished> {
        TEvUploadFinished(const TString& key, const TString& url, ui64 uploadSize) : Key(key), Url(url), UploadSize(uploadSize) {}
        const TString Key, Url;
        const ui64 UploadSize;
    };

    struct TEvUploadError : public TEventLocal<TEvUploadError, EvUploadError> {

        TEvUploadError(long httpCode, const TString& s3ErrorCode, const TString& message)
        : StatusCode(NYql::NDqProto::StatusIds::UNSPECIFIED), HttpCode(httpCode), S3ErrorCode(s3ErrorCode), Message(message) {
            BuildIssues();
        }

        TEvUploadError(const TString& s3ErrorCode, const TString& message)
        : StatusCode(NYql::NDqProto::StatusIds::UNSPECIFIED), HttpCode(0), S3ErrorCode(s3ErrorCode), Message(message) {
            BuildIssues();
        }

        TEvUploadError(NYql::NDqProto::StatusIds::StatusCode statusCode, const TString& message)
        : StatusCode(statusCode), HttpCode(0), Message(message) {
            BuildIssues();
        }

        TEvUploadError(long httpCode, const TString& message)
        : StatusCode(NYql::NDqProto::StatusIds::UNSPECIFIED), HttpCode(httpCode), Message(message) {
            BuildIssues();
        }

        TEvUploadError(TIssues&& issues)
        : StatusCode(NYql::NDqProto::StatusIds::UNSPECIFIED), HttpCode(0), Issues(issues) {
            // don't build
        }

        void BuildIssues() {
            Issues = ::NYql::NDq::BuildIssues(HttpCode, S3ErrorCode, Message);
        }

        NYql::NDqProto::StatusIds::StatusCode StatusCode;
        long HttpCode;
        TString S3ErrorCode;
        TString Message;
        TIssues Issues;
    };

    struct TEvUploadStarted : public TEventLocal<TEvUploadStarted, EvUploadStarted> {
        explicit TEvUploadStarted(TString&& uploadId) : UploadId(std::move(uploadId)) {}
        const TString UploadId;
    };

    struct TEvUploadPartFinished : public TEventLocal<TEvUploadPartFinished, EvUploadPartFinished> {
        TEvUploadPartFinished(size_t size, size_t index, TString&& etag) : Size(size), Index(index), ETag(std::move(etag)) {}
        const size_t Size, Index;
        const TString ETag;
    };
};

using namespace NKikimr::NMiniKQL;

class TS3FileWriteActor : public TActorBootstrapped<TS3FileWriteActor> {
    friend class TS3WriteActor;

public:
    TS3FileWriteActor(
        const TTxId& txId,
        IHTTPGateway::TPtr gateway,
        NYdb::TCredentialsProviderPtr credProvider,
        const TString& key, const TString& url, const std::string_view& compression,
        const IRetryPolicy<long>::TPtr& retryPolicy)
        : TxId(txId)
        , Gateway(std::move(gateway))
        , CredProvider(std::move(credProvider))
        , RetryPolicy(retryPolicy)
        , ActorSystem(TActivationContext::ActorSystem())
        , Key(key)
        , Url(url)
        , RequestId(CreateGuidAsString())
        , Parts(MakeCompressorQueue(compression))
    {
        YQL_ENSURE(Parts, "Compression '" << compression << "' is not supported.");
    }

    void Bootstrap(const TActorId& parentId) {
        ParentId = parentId;
        LOG_D("TS3FileWriteActor", "Bootstrap by " << ParentId << " for Key: [" << Key << "], Url: [" << Url << "], request id: [" << RequestId << "]");
        if (Parts->IsSealed() && Parts->Size() <= 1) {
            Become(&TS3FileWriteActor::SinglepartWorkingStateFunc);
            const size_t size = Max<size_t>(Parts->Volume(), 1);
            InFlight += size;
            SentSize += size;
            Gateway->Upload(Url, MakeHeaders(RequestId), Parts->Pop(), std::bind(&TS3FileWriteActor::OnUploadFinish, ActorSystem, SelfId(), ParentId, Key, Url, RequestId, size, std::placeholders::_1), true, RetryPolicy);
        } else {
            Become(&TS3FileWriteActor::MultipartInitialStateFunc);
            Gateway->Upload(Url + "?uploads", MakeHeaders(RequestId), 0, std::bind(&TS3FileWriteActor::OnUploadsCreated, ActorSystem, SelfId(), ParentId, RequestId, std::placeholders::_1), false, RetryPolicy);
        }
    }

    static constexpr char ActorName[] = "S3_FILE_WRITE_ACTOR";

    void Handle(TEvPrivate::TEvUploadFinished::TPtr& ev) {
        InFlight -= ev->Get()->UploadSize;
    }

    void PassAway() override {
        if (InFlight || !Parts->Empty()) {
            LOG_W("TS3FileWriteActor", "PassAway: but NOT finished, InFlight: " << InFlight << ", Parts: " << Parts->Size() << ", Sealed: " << Parts->IsSealed() << ", request id: [" << RequestId << "]");
        } else {
            LOG_D("TS3FileWriteActor", "PassAway: request id: [" << RequestId << "]");
        }
        TActorBootstrapped<TS3FileWriteActor>::PassAway();
    }

    void AddData(TString&& data) {
        Parts->Push(std::move(data));
    }

    void Seal() {
        Parts->Seal();
    }

    void Go() {
        if (!UploadId.empty())
            StartUploadParts();
    }

    void Finish() {
        if (IsFinishing())
            return;

        Parts->Seal();

        if (!UploadId.empty()) {
            if (!Parts->Empty())
                StartUploadParts();
            else if (!InFlight && Parts->Empty())
                CommitUploadedParts();
        }
    }

    bool IsFinishing() const { return Parts->IsSealed(); }

    const TString& GetUrl() const { return Url; }

    i64 GetMemoryUsed() const {
        return InFlight + Parts->Volume();
    }
private:
    STRICT_STFUNC(MultipartInitialStateFunc,
        hFunc(TEvPrivate::TEvUploadStarted, Handle);
    )

    STRICT_STFUNC(MultipartWorkingStateFunc,
        hFunc(TEvPrivate::TEvUploadPartFinished, Handle);
    )

    STRICT_STFUNC(SinglepartWorkingStateFunc,
        hFunc(TEvPrivate::TEvUploadFinished, Handle);
    )

    static void OnUploadsCreated(TActorSystem* actorSystem, TActorId selfId, TActorId parentId, const TString& requestId, IHTTPGateway::TResult&& result) {
        switch (result.index()) {
        case 0U: try {
            const NXml::TDocument xml(std::get<IHTTPGateway::TContent>(std::move(result)).Extract(), NXml::TDocument::String);
            if (const auto& root = xml.Root(); root.Name() == "Error") {
                const auto& code = root.Node("Code", true).Value<TString>();
                const auto& message = root.Node("Message", true).Value<TString>();
                actorSystem->Send(new IEventHandle(parentId, selfId, new TEvPrivate::TEvUploadError(code, TStringBuilder{} << message << ", request id: [" << requestId << "]")));
            } else if (root.Name() != "InitiateMultipartUploadResult")
                actorSystem->Send(new IEventHandle(parentId, selfId, new TEvPrivate::TEvUploadError(NYql::NDqProto::StatusIds::INTERNAL_ERROR, TStringBuilder() << "Unexpected response on create upload: " << root.Name() << ", request id: [" << requestId << "]")));
            else {
                const NXml::TNamespacesForXPath nss(1U, {"s3", "http://s3.amazonaws.com/doc/2006-03-01/"});
                actorSystem->Send(new IEventHandle(selfId, selfId, new TEvPrivate::TEvUploadStarted(root.Node("s3:UploadId", false, nss).Value<TString>())));
            }
            break;
        } catch (const std::exception& ex) {
            actorSystem->Send(new IEventHandle(parentId, selfId, new TEvPrivate::TEvUploadError(NYql::NDqProto::StatusIds::INTERNAL_ERROR, TStringBuilder() << "Error on parse create upload response: " << ex.what()  << ", request id: [" << requestId << "]")));
            break;
        }
        case 1U: {
            auto issues = NS3Util::AddParentIssue(TStringBuilder{} << "Upload error, request id: [" << requestId << "], ", std::get<TIssues>(std::move(result)));
            actorSystem->Send(new IEventHandle(parentId, selfId, new TEvPrivate::TEvUploadError(std::move(issues))));
            break;
        }
        default:
            actorSystem->Send(new IEventHandle(parentId, selfId, new TEvPrivate::TEvUploadError(NYql::NDqProto::StatusIds::INTERNAL_ERROR, TStringBuilder() << "Unexpected variant index " << result.index() << " on create upload response." << ", request id: [" << requestId << "]")));
            break;
        }
    }

    static void OnPartUploadFinish(TActorSystem* actorSystem, TActorId selfId, TActorId parentId, size_t size, size_t index, const TString& requestId, IHTTPGateway::TResult&& response) {
        switch (response.index()) {
        case 0U: {
            const auto& str = std::get<IHTTPGateway::TContent>(response).Headers;
            if (const NHttp::THeaders headers(str.substr(str.rfind("HTTP/"))); headers.Has("Etag"))
                actorSystem->Send(new IEventHandle(selfId, selfId, new TEvPrivate::TEvUploadPartFinished(size, index, TString(headers.Get("Etag")))));
            else
                actorSystem->Send(new IEventHandle(parentId, selfId, new TEvPrivate::TEvUploadError(NYql::NDqProto::StatusIds::INTERNAL_ERROR, TStringBuilder() << "Unexpected response: " << str << ", request id: [" << requestId << "]")));
            }
            break;
        case 1U: {
            auto issues = NS3Util::AddParentIssue(TStringBuilder{} << "PartUpload error, request id: [" << requestId << "], ", std::get<TIssues>(std::move(response)));
            actorSystem->Send(new IEventHandle(parentId, selfId, new TEvPrivate::TEvUploadError(std::move(issues))));
            break;
        }
        }
    }

    static void OnMultipartUploadFinish(TActorSystem* actorSystem, TActorId selfId, TActorId parentId, const TString& key, const TString& url, const TString& requestId, ui64 sentSize, IHTTPGateway::TResult&& result) {
        switch (result.index()) {
        case 0U: try {
            const NXml::TDocument xml(std::get<IHTTPGateway::TContent>(std::move(result)).Extract(), NXml::TDocument::String);
            if (const auto& root = xml.Root(); root.Name() == "Error") {
                const auto& code = root.Node("Code", true).Value<TString>();
                const auto& message = root.Node("Message", true).Value<TString>();
                actorSystem->Send(new IEventHandle(parentId, selfId, new TEvPrivate::TEvUploadError(code, TStringBuilder{} << message << ", request id: [" << requestId << "]")));
            } else if (root.Name() != "CompleteMultipartUploadResult")
                actorSystem->Send(new IEventHandle(parentId, selfId, new TEvPrivate::TEvUploadError(NYql::NDqProto::StatusIds::INTERNAL_ERROR, TStringBuilder() << "Unexpected response on finish upload: " << root.Name() << ", request id: [" << requestId << "]")));
            else
                actorSystem->Send(new IEventHandle(parentId, selfId, new TEvPrivate::TEvUploadFinished(key, url, sentSize)));
            break;
        } catch (const std::exception& ex) {
            actorSystem->Send(new IEventHandle(parentId, selfId, new TEvPrivate::TEvUploadError(NYql::NDqProto::StatusIds::INTERNAL_ERROR, TStringBuilder() << "Error on parse finish upload response: " << ex.what() << ", request id: [" << requestId << "]")));
            break;
        }
        case 1U: {
            auto issues = NS3Util::AddParentIssue(TStringBuilder{} << "Multipart error, request id: [" << requestId << "], ", std::get<TIssues>(std::move(result)));
            actorSystem->Send(new IEventHandle(parentId, selfId, new TEvPrivate::TEvUploadError(std::move(issues))));
            break;
        }
        default:
            actorSystem->Send(new IEventHandle(parentId, selfId, new TEvPrivate::TEvUploadError(NYql::NDqProto::StatusIds::INTERNAL_ERROR, TStringBuilder() << "Unexpected variant index " << result.index() << " on finish upload response." << ", request id: [" << requestId << "]")));
            break;
        }
    }

    static void OnUploadFinish(TActorSystem* actorSystem, TActorId selfId, TActorId parentId, const TString& key, const TString& url, const TString& requestId, ui64 sentSize, IHTTPGateway::TResult&& result) {
        switch (result.index()) {
        case 0U:
            if (auto content = std::get<IHTTPGateway::TContent>(std::move(result)); content.HttpResponseCode >= 300) {
                TString errorText = content.Extract();
                TString errorCode;
                TString message;
                if (ParseS3ErrorResponse(errorText, errorCode, message)) {
                    actorSystem->Send(new IEventHandle(parentId, selfId, new TEvPrivate::TEvUploadError(content.HttpResponseCode, errorCode, TStringBuilder{} << message << ", request id: [" << requestId << "]")));
                } else {
                    actorSystem->Send(new IEventHandle(parentId, selfId, new TEvPrivate::TEvUploadError(content.HttpResponseCode, TStringBuilder{} << errorText << ", request id: [" << requestId << "]")));
                }
            } else {
                actorSystem->Send(new IEventHandle(selfId, selfId, new TEvPrivate::TEvUploadFinished(key, url, sentSize)));
                actorSystem->Send(new IEventHandle(parentId, selfId, new TEvPrivate::TEvUploadFinished(key, url, sentSize)));
            }
            break;
        case 1U: {
            auto issues = NS3Util::AddParentIssue(TStringBuilder{} << "UploadFinish error, request id: [" << requestId << "], ", std::get<TIssues>(std::move(result)));
            actorSystem->Send(new IEventHandle(parentId, selfId, new TEvPrivate::TEvUploadError(std::move(issues))));
            break;
        }
        default:
            actorSystem->Send(new IEventHandle(parentId, selfId, new TEvPrivate::TEvUploadError(NYql::NDqProto::StatusIds::INTERNAL_ERROR, TStringBuilder() << "Unexpected variant index " << result.index() << " on finish upload response." << ", request id: [" << requestId << "]")));
            break;
        }
    }

    void Handle(TEvPrivate::TEvUploadStarted::TPtr& result) {
        UploadId = result->Get()->UploadId;
        Become(&TS3FileWriteActor::MultipartWorkingStateFunc);
        StartUploadParts();
    }

    void Handle(TEvPrivate::TEvUploadPartFinished::TPtr& result) {
        InFlight -= result->Get()->Size;
        Tags[result->Get()->Index] = std::move(result->Get()->ETag);

        if (!InFlight && Parts->IsSealed() && Parts->Empty())
            CommitUploadedParts();
    }

    void StartUploadParts() {
        while (auto part = Parts->Pop()) {
            const auto size = part.size();
            const auto index = Tags.size();
            Tags.emplace_back();
            InFlight += size;
            SentSize += size;
            Gateway->Upload(Url + "?partNumber=" + std::to_string(index + 1) + "&uploadId=" + UploadId, MakeHeaders(RequestId), std::move(part), std::bind(&TS3FileWriteActor::OnPartUploadFinish, ActorSystem, SelfId(), ParentId, size, index, RequestId, std::placeholders::_1), true, RetryPolicy);
        }
    }

    void CommitUploadedParts() {
        Become(nullptr);
        TStringBuilder xml;
        xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" << Endl;
        xml << "<CompleteMultipartUpload xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">" << Endl;
        size_t i = 0U;
        for (const auto& tag : Tags)
            xml << "<Part><PartNumber>" << ++i << "</PartNumber><ETag>" << tag << "</ETag></Part>" << Endl;
        xml << "</CompleteMultipartUpload>" << Endl;
        Gateway->Upload(Url + "?uploadId=" + UploadId, MakeHeaders(RequestId), xml, std::bind(&TS3FileWriteActor::OnMultipartUploadFinish, ActorSystem, SelfId(), ParentId, Key, Url, RequestId, SentSize, std::placeholders::_1), false, RetryPolicy);
    }

    IHTTPGateway::THeaders MakeHeaders(const TString& requestId) const {
        if (const auto& token = CredProvider->GetAuthInfo(); token.empty())
            return IHTTPGateway::THeaders{TString{"X-Request-ID:"} += requestId};
        else
            return IHTTPGateway::THeaders{TString("X-YaCloud-SubjectToken:") += token, TString{"X-Request-ID:"} += requestId};
    }

    size_t InFlight = 0ULL;
    size_t SentSize = 0ULL;

    const TTxId TxId;
    const IHTTPGateway::TPtr Gateway;
    const NYdb::TCredentialsProviderPtr CredProvider;
    const IRetryPolicy<long>::TPtr RetryPolicy;

    TActorSystem* const ActorSystem;
    TActorId ParentId;

    const TString Key;
    const TString Url;
    const TString RequestId;

    IOutputQueue::TPtr Parts;
    std::vector<TString> Tags;

    TString UploadId;
};

class TS3WriteActor : public TActorBootstrapped<TS3WriteActor>, public IDqComputeActorAsyncOutput {
public:
    TS3WriteActor(ui64 outputIndex,
        const TTxId& txId,
        const TString& prefix,
        IHTTPGateway::TPtr gateway,
        NYdb::TCredentialsProviderPtr credProvider,
        IRandomProvider* randomProvider,
        const TString& url,
        const TString& path,
        const TString& extension,
        const std::vector<TString>& keys,
        const size_t memoryLimit,
        const TString& compression,
        bool multipart,
        IDqComputeActorAsyncOutput::ICallbacks* callbacks,
        const IRetryPolicy<long>::TPtr& retryPolicy)
        : Gateway(std::move(gateway))
        , CredProvider(std::move(credProvider))
        , RandomProvider(randomProvider)
        , RetryPolicy(retryPolicy)
        , OutputIndex(outputIndex)
        , TxId(txId)
        , Prefix(prefix)
        , Callbacks(callbacks)
        , Url(url)
        , Path(path)
        , Extension(extension)
        , Keys(keys)
        , MemoryLimit(memoryLimit)
        , Compression(compression)
        , Multipart(multipart)
    {
        if (!RandomProvider) {
            DefaultRandomProvider = CreateDefaultRandomProvider();
            RandomProvider = DefaultRandomProvider.Get();
        }
    }

    void Bootstrap() {
        LOG_D("TS3WriteActor", "Bootstrap");
        Become(&TS3WriteActor::StateFunc);
    }

    static constexpr char ActorName[] = "S3_WRITE_ACTOR";
private:
    void CommitState(const NDqProto::TCheckpoint&) final {};
    void LoadState(const NDqProto::TSinkState&) final {};
    ui64 GetOutputIndex() const final { return OutputIndex; }
    i64 GetFreeSpace() const final {
        return std::accumulate(FileWriteActors.cbegin(), FileWriteActors.cend(), i64(MemoryLimit),
            [](i64 free, const std::pair<const TString, std::vector<TS3FileWriteActor*>>& item) {
                return free - std::accumulate(item.second.cbegin(), item.second.cend(), i64(0), [](i64 sum, TS3FileWriteActor* actor) { return sum += actor->GetMemoryUsed(); });
            });
    }

    TString MakePartitionKey(const NUdf::TUnboxedValuePod v) const {
        if (Keys.empty())
            return {};

        auto elements = v.GetElements();
        TStringBuilder key;
        for (const auto& k : Keys) {
            const std::string_view keyPart = (++elements)->AsStringRef();
            YQL_ENSURE(std::string_view::npos == keyPart.find('/'), "Invalid partition key, contains '/': " << keyPart);
            key << k << '=' << keyPart << '/';
        }
        return UrlEscapeRet(key);
    }

    TString MakeOutputName() const {
        const auto rand = std::make_tuple(RandomProvider->GenUuid4(), RandomProvider->GenRand());
        return Prefix + Base64EncodeUrl(TStringBuf(reinterpret_cast<const char*>(&rand), sizeof(rand)));
    }

    STRICT_STFUNC(StateFunc,
        hFunc(TEvPrivate::TEvUploadError, Handle);
        hFunc(TEvPrivate::TEvUploadFinished, Handle);
    )

    void SendData(TUnboxedValueVector&& data, i64, const TMaybe<NDqProto::TCheckpoint>&, bool finished) final {
        std::unordered_set<TS3FileWriteActor*> processedActors;
        for (const auto& v : data) {
            const auto& key = MakePartitionKey(v);
            const auto [keyIt, insertedNew] = FileWriteActors.emplace(key, std::vector<TS3FileWriteActor*>());
            if (insertedNew || keyIt->second.empty() || keyIt->second.back()->IsFinishing()) {
                auto fileWrite = std::make_unique<TS3FileWriteActor>(TxId, Gateway, CredProvider, key, Url + Path + key + MakeOutputName() + Extension, Compression, RetryPolicy);
                keyIt->second.emplace_back(fileWrite.get());
                RegisterWithSameMailbox(fileWrite.release());
            }

            const NUdf::TUnboxedValue& value = Keys.empty() ? v : *v.GetElements();
            TS3FileWriteActor* actor = keyIt->second.back();
            if (value) {
                actor->AddData(TString(value.AsStringRef()));
            }
            if (!Multipart || !value) {
                actor->Seal();
            }
            processedActors.insert(actor);
        }

        for (TS3FileWriteActor* actor : processedActors) {
            actor->Go();
        }

        if (finished) {
            std::for_each(
                FileWriteActors.cbegin(),
                FileWriteActors.cend(),
                [](const std::pair<const TString, std::vector<TS3FileWriteActor*>>& item) {
                    item.second.back()->Finish();
                });
            Finished = true;
            FinishIfNeeded();
        }
        data.clear();
    }

    ui64 GetEgressBytes() override {
        return EgressBytes;
    }

    void Handle(TEvPrivate::TEvUploadError::TPtr& result) {
        LOG_W("TS3WriteActor", "TEvUploadError " << result->Get()->Issues.ToOneLineString());

        auto statusCode = result->Get()->StatusCode;
        if (statusCode == NYql::NDqProto::StatusIds::UNSPECIFIED) {

            // add err code analysis here

            if (result->Get()->S3ErrorCode == "BucketMaxSizeExceeded") {
                statusCode = NYql::NDqProto::StatusIds::LIMIT_EXCEEDED;
            } else {
                statusCode = NYql::NDqProto::StatusIds::EXTERNAL_ERROR;
            }
        }

        Callbacks->OnAsyncOutputError(OutputIndex, result->Get()->Issues, statusCode);
    }

    void FinishIfNeeded() {
        if (FileWriteActors.empty() && Finished) {
            LOG_D("TS3WriteActor", "Finished, notify owner");
            Callbacks->OnAsyncOutputFinished(OutputIndex);
        }
    }

    void Handle(TEvPrivate::TEvUploadFinished::TPtr& result) {
        if (const auto it = FileWriteActors.find(result->Get()->Key); FileWriteActors.cend() != it) {
            EgressBytes += result->Get()->UploadSize;
            if (const auto ft = std::find_if(it->second.cbegin(), it->second.cend(), [&](TS3FileWriteActor* actor){ return result->Get()->Url == actor->GetUrl(); }); it->second.cend() != ft) {
                (*ft)->PassAway();
                it->second.erase(ft);
                if (it->second.empty())
                    FileWriteActors.erase(it);
            }
        }
        FinishIfNeeded();
    }

    // IActor & IDqComputeActorAsyncOutput
    void PassAway() override { // Is called from Compute Actor
        ui32 fileWriterCount = 0;
        for (const auto& p : FileWriteActors) {
            for (const auto& fileWriter : p.second) {
                fileWriter->PassAway();
                fileWriterCount++;
            }
        }
        FileWriteActors.clear();

        if (fileWriterCount) {
            LOG_W("TS3WriteActor", "PassAway: " << " with " << fileWriterCount << " NOT finished FileWriter(s)");
        } else {
            LOG_D("TS3WriteActor", "PassAway");
        }

        TActorBootstrapped<TS3WriteActor>::PassAway();
    }

    const IHTTPGateway::TPtr Gateway;
    const NYdb::TCredentialsProviderPtr CredProvider;
    IRandomProvider* RandomProvider;
    TIntrusivePtr<IRandomProvider> DefaultRandomProvider;
    const IRetryPolicy<long>::TPtr RetryPolicy;

    const ui64 OutputIndex;
    const TTxId TxId;
    const TString Prefix;
    IDqComputeActorAsyncOutput::ICallbacks *const Callbacks;

    const TString Url;
    const TString Path;
    const TString Extension;
    const std::vector<TString> Keys;

    const size_t MemoryLimit;
    const TString Compression;
    const bool Multipart;
    bool Finished = false;
    ui64 EgressBytes = 0;

    std::unordered_map<TString, std::vector<TS3FileWriteActor*>> FileWriteActors;
};

} // namespace

std::pair<IDqComputeActorAsyncOutput*, NActors::IActor*> CreateS3WriteActor(
    const NKikimr::NMiniKQL::TTypeEnvironment&,
    const NKikimr::NMiniKQL::IFunctionRegistry&,
    IRandomProvider* randomProvider,
    IHTTPGateway::TPtr gateway,
    NS3::TSink&& params,
    ui64 outputIndex,
    const TTxId& txId,
    const TString& prefix,
    const THashMap<TString, TString>& secureParams,
    IDqComputeActorAsyncOutput::ICallbacks* callbacks,
    ISecuredServiceAccountCredentialsFactory::TPtr credentialsFactory,
    const IRetryPolicy<long>::TPtr& retryPolicy)
{
    const auto token = secureParams.Value(params.GetToken(), TString{});
    const auto credentialsProviderFactory = CreateCredentialsProviderFactoryForStructuredToken(credentialsFactory, token);
    const auto actor = new TS3WriteActor(
        outputIndex,
        txId,
        prefix,
        std::move(gateway),
        credentialsProviderFactory->CreateProvider(),
        randomProvider, params.GetUrl(),
        params.GetPath(),
        params.GetExtension(),
        std::vector<TString>(params.GetKeys().cbegin(), params.GetKeys().cend()),
        params.HasMemoryLimit() ? params.GetMemoryLimit() : 1_GB,
        params.GetCompression(),
        params.GetMultipart(),
        callbacks,
        retryPolicy);
    return {actor, actor};
}

} // namespace NYql::NDq
