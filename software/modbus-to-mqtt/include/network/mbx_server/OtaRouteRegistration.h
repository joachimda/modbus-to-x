#ifndef MODBUS_TO_MQTT_OTAROUTEREGISTRATION_H
#define MODBUS_TO_MQTT_OTAROUTEREGISTRATION_H

#include <cstddef>
#include <cstdint>

#include "constants/Routes.h"

/**
 * @file OtaRouteRegistration.h
 * @brief Generic registration of OTA, credential, and factory-reset HTTP routes.
 */

/**
 * @brief Route-wiring helpers shared by the production server and native tests.
 *
 * Platform-specific request processing is supplied through a callback adapter;
 * this namespace owns route selection, authorization filters, fallback
 * handlers, and upload/body dispatch.
 */
namespace OtaRouteRegistration {

/**
 * @brief Registers all OTA-related routes on a server.
 *
 * Protected routes receive an authorization filter followed by an
 * unauthorized fallback handler. Upload authorization uses
 * `cacheUploadAuthorization()` so the decision can be reused for subsequent
 * chunks. The HTTP OTA settings GET and POST routes are registered without an
 * authorization filter.
 *
 * @par Callback adapter requirements
 * @p Callbacks must be copyable and provide the following operations:
 * @code{.cpp}
 * bool isAuthorized(const Request *request) const;
 * bool cacheUploadAuthorization(Request *request) const;
 * void sendUnauthorized(Request *request) const;
 * void logRequest(const Request *request) const;
 * void handleFirmwareUpload(Request *, const Filename &, size_t,
 *                           uint8_t *, size_t, bool) const;
 * void handleFilesystemUpload(Request *, const Filename &, size_t,
 *                             uint8_t *, size_t, bool) const;
 * void handleHttpCheck(Request *request) const;
 * void handleHttpNotes(Request *request) const;
 * void handleHttpApply(Request *request) const;
 * void handleGetHttpSettings(Request *request) const;
 * void handlePutHttpSettingsBody(Request *, const uint8_t *, size_t,
 *                                size_t, size_t) const;
 * void handlePutPasswordBody(Request *, const uint8_t *, size_t,
 *                            size_t, size_t) const;
 * void handleDeletePassword(Request *request) const;
 * void handleFactoryResetBody(Request *, const uint8_t *, size_t,
 *                             size_t, size_t) const;
 * @endcode
 *
 * @tparam Request Server request type. It must expose `method()` and `url()`.
 * @tparam Filename Filename type passed to upload handlers.
 * @tparam Server Server type compatible with the ESPAsyncWebServer `on()` and
 *         route `setFilter()` interfaces.
 * @tparam Callbacks Platform-specific callback adapter described above.
 * @tparam Method HTTP method identifier type.
 * @param[in,out] server Server on which the routes are registered.
 * @param[in] callbacks Callback adapter copied into each registered handler.
 * @param[in] get Identifier for the HTTP GET method.
 * @param[in] post Identifier for the HTTP POST method.
 * @param[in] put Identifier for the HTTP PUT method.
 * @param[in] remove Identifier for the HTTP DELETE method.
 * @pre @p server must not be `nullptr`.
 */
template<typename Request, typename Filename, typename Server, typename Callbacks, typename Method>
void configure(Server *server, const Callbacks callbacks,
               const Method get, const Method post, const Method put, const Method remove) {
    const auto authorized = [callbacks](const char *route, const Method method) {
        return [callbacks, route, method](Request *request) {
            return request != nullptr && request->method() == method && request->url() == route
                   && callbacks.isAuthorized(request);
        };
    };
    const auto authorizedUpload = [callbacks, post](const char *route) {
        return [callbacks, route, post](Request *request) {
            return request != nullptr && request->method() == post && request->url() == route
                   && callbacks.cacheUploadAuthorization(request);
        };
    };

    auto &firmware = server->on(Routes::OTA_FIRMWARE, post, [callbacks](Request *request) {
        callbacks.logRequest(request);
    }, [callbacks](Request *request, const Filename &filename, const size_t index,
                   uint8_t *data, const size_t length, const bool final) {
        callbacks.handleFirmwareUpload(request, filename, index, data, length, final);
    });
    firmware.setFilter(authorizedUpload(Routes::OTA_FIRMWARE));
    server->on(Routes::OTA_FIRMWARE, post, [callbacks](Request *request) {
        callbacks.sendUnauthorized(request);
    }, [callbacks](Request *request, const Filename &, const size_t index, uint8_t *, const size_t, const bool) {
        if (index == 0U) callbacks.sendUnauthorized(request);
    });

    auto &filesystem = server->on(Routes::OTA_FILESYSTEM, post, [callbacks](Request *request) {
        callbacks.logRequest(request);
    }, [callbacks](Request *request, const Filename &filename, const size_t index,
                   uint8_t *data, const size_t length, const bool final) {
        callbacks.handleFilesystemUpload(request, filename, index, data, length, final);
    });
    filesystem.setFilter(authorizedUpload(Routes::OTA_FILESYSTEM));
    server->on(Routes::OTA_FILESYSTEM, post, [callbacks](Request *request) {
        callbacks.sendUnauthorized(request);
    }, [callbacks](Request *request, const Filename &, const size_t index, uint8_t *, const size_t, const bool) {
        if (index == 0U) callbacks.sendUnauthorized(request);
    });

    auto &check = server->on(Routes::OTA_HTTP_CHECK, post, [callbacks](Request *request) {
        callbacks.logRequest(request);
        callbacks.handleHttpCheck(request);
    });
    check.setFilter(authorized(Routes::OTA_HTTP_CHECK, post));
    server->on(Routes::OTA_HTTP_CHECK, post, [callbacks](Request *request) {
        callbacks.sendUnauthorized(request);
    });

    auto &notes = server->on(Routes::OTA_HTTP_NOTES, post, [callbacks](Request *request) {
        callbacks.logRequest(request);
        callbacks.handleHttpNotes(request);
    });
    notes.setFilter(authorized(Routes::OTA_HTTP_NOTES, post));
    server->on(Routes::OTA_HTTP_NOTES, post, [callbacks](Request *request) {
        callbacks.sendUnauthorized(request);
    });

    auto &apply = server->on(Routes::OTA_HTTP_APPLY, post, [callbacks](Request *request) {
        callbacks.logRequest(request);
        callbacks.handleHttpApply(request);
    });
    apply.setFilter(authorized(Routes::OTA_HTTP_APPLY, post));
    server->on(Routes::OTA_HTTP_APPLY, post, [callbacks](Request *request) {
        callbacks.sendUnauthorized(request);
    });

    server->on(Routes::OTA_HTTP_SETTINGS, get, [callbacks](Request *request) {
        callbacks.logRequest(request);
        callbacks.handleGetHttpSettings(request);
    });
    server->on(Routes::OTA_HTTP_SETTINGS, post, [callbacks](const Request *request) {
        callbacks.logRequest(request);
    }, nullptr, [callbacks](Request *request, const uint8_t *data, const size_t length,
                            const size_t index, const size_t total) {
        callbacks.handlePutHttpSettingsBody(request, data, length, index, total);
    });

    auto &setPassword = server->on(Routes::OTA_PASSWORD, put, [callbacks](const Request *request) {
        callbacks.logRequest(request);
    }, nullptr, [callbacks](Request *request, const uint8_t *data, const size_t length,
                            const size_t index, const size_t total) {
        callbacks.handlePutPasswordBody(request, data, length, index, total);
    });
    setPassword.setFilter(authorized(Routes::OTA_PASSWORD, put));
    server->on(Routes::OTA_PASSWORD, put, [callbacks](Request *request) {
        callbacks.sendUnauthorized(request);
    });

    auto &deletePassword = server->on(Routes::OTA_PASSWORD, remove, [callbacks](Request *request) {
        callbacks.logRequest(request);
        callbacks.handleDeletePassword(request);
    });
    deletePassword.setFilter(authorized(Routes::OTA_PASSWORD, remove));
    server->on(Routes::OTA_PASSWORD, remove, [callbacks](Request *request) {
        callbacks.sendUnauthorized(request);
    });

    auto &factoryReset = server->on(Routes::FACTORY_RESET, post, [callbacks](const Request *request) {
        callbacks.logRequest(request);
    }, nullptr, [callbacks](Request *request, const uint8_t *data, const size_t length,
                            const size_t index, const size_t total) {
        callbacks.handleFactoryResetBody(request, data, length, index, total);
    });
    factoryReset.setFilter(authorized(Routes::FACTORY_RESET, post));
    server->on(Routes::FACTORY_RESET, post, [callbacks](Request *request) {
        callbacks.sendUnauthorized(request);
    });
}

}  // namespace OtaRouteRegistration

#endif
