#ifndef MODBUS_TO_MQTT_OTAUPLOADGUARDCORE_H
#define MODBUS_TO_MQTT_OTAUPLOADGUARDCORE_H

#include <cstddef>
#include <cstdint>

/**
 * @file OtaUploadGuardCore.h
 * @brief Platform-independent authorization and start-state guard for OTA uploads.
 */

/** @brief State and callbacks used to guard a chunked OTA upload. */
namespace OtaUploadGuardCore {

/**
 * @brief Per-request authorization and upload-start state.
 *
 * Value-initialize this structure before its first use. Authorization is
 * evaluated once by @ref authorize and cached for the lifetime of the state.
 */
struct State {
    /** Whether an authorization decision has been cached. */
    bool authorizationChecked;

    /** Cached authorization decision. */
    bool authorized;

    /** Whether the guarded OTA operation has started successfully. */
    bool begun;
};

/** @brief Result of attempting to start a guarded OTA operation. */
enum class StartResult : uint8_t {
    /** Authorization has not been checked or the cached decision denied access. */
    Unauthorized,

    /** The begin callback was missing or reported failure. */
    BeginFailed,

    /** The begin callback succeeded and the state is ready for writes. */
    Started,
};

/**
 * @brief Callback that starts the underlying OTA operation.
 *
 * @param[in,out] context Opaque caller-provided context.
 * @return `true` if the operation started successfully; otherwise `false`.
 */
using BeginFunction = bool (*)(void *context);

/**
 * @brief Callback that writes one chunk to the underlying OTA operation.
 *
 * @param[in,out] context Opaque caller-provided context.
 * @param[in,out] data Upload data. The callback may consume or modify this buffer.
 * @param[in] length Number of bytes in @p data.
 * @return `true` if the complete chunk was written; otherwise `false`.
 */
using WriteFunction = bool (*)(void *context, uint8_t *data, size_t length);

/**
 * @brief Callback that authorizes the request associated with an upload.
 *
 * @param[in,out] context Opaque caller-provided context.
 * @return `true` if the request is authorized; otherwise `false`.
 */
using AuthorizeFunction = bool (*)(void *context);

/**
 * @brief Evaluates and caches authorization for an upload request.
 *
 * On the first call, the state is marked as checked, @p authorizeRequest is
 * invoked, and the upload-start flag is reset. Later calls return the cached
 * decision without invoking the callback or modifying the state.
 *
 * @param[in,out] state Value-initialized per-request upload state.
 * @param[in] authorizeRequest Authorization callback. A null callback denies access.
 * @param[in,out] context Opaque value passed to @p authorizeRequest.
 * @return The newly evaluated or previously cached authorization decision.
 */
inline bool authorize(State &state, const AuthorizeFunction authorizeRequest, void *context) {
    if (state.authorizationChecked) return state.authorized;
    state.authorizationChecked = true;
    state.authorized = authorizeRequest != nullptr && authorizeRequest(context);
    state.begun = false;
    return state.authorized;
}

/**
 * @brief Starts an authorized OTA operation.
 *
 * @param[in,out] state Upload state previously passed to @ref authorize.
 * @param[in] begin Callback that starts the underlying operation.
 * @param[in,out] context Opaque value passed to @p begin.
 * @return @ref StartResult::Unauthorized if authorization is missing or was
 *         denied, @ref StartResult::BeginFailed if the callback is null or
 *         fails, or @ref StartResult::Started on success.
 * @note A failed start leaves the existing @ref State::begun value unchanged.
 */
inline StartResult start(State &state, const BeginFunction begin, void *context) {
    if (!state.authorizationChecked || !state.authorized) return StartResult::Unauthorized;
    if (begin == nullptr || !begin(context)) return StartResult::BeginFailed;
    state.begun = true;
    return StartResult::Started;
}

/**
 * @brief Writes one chunk after authorization and successful startup.
 *
 * A zero-length chunk succeeds without invoking @p writer. This function does
 * not modify @p state.
 *
 * @param[in] state Current upload state.
 * @param[in] writer Callback that writes a non-empty chunk.
 * @param[in,out] context Opaque value passed to @p writer.
 * @param[in,out] data Upload buffer passed to @p writer.
 * @param[in] length Number of bytes in @p data.
 * @return `true` for an authorized, started zero-length write or when
 *         @p writer accepts the complete chunk; otherwise `false`.
 */
inline bool write(State &state, const WriteFunction writer, void *context, uint8_t *data, const size_t length) {
    if (!state.authorized || !state.begun || writer == nullptr) return false;
    return length == 0U || writer(context, data, length);
}

}  // namespace OtaUploadGuardCore

#endif
