#ifndef MODBUS_TO_MQTT_MUTATIONROUTEREGISTRATION_H
#define MODBUS_TO_MQTT_MUTATIONROUTEREGISTRATION_H

#include <cstddef>
#include <cstdint>

#include "constants/Routes.h"

namespace MutationRouteRegistration {

enum class Mode {
    Station,
    AccessPoint,
};

template<typename Request, typename Server, typename Callbacks, typename Method, typename Handler>
void addRequest(Server *server, const Callbacks callbacks, const char *route,
                const Method method, const Handler handler) {
    auto &entry = server->on(route, method, [callbacks, handler](Request *request) {
        callbacks.logRequest(request);
        handler(callbacks, request);
    });
    entry.setFilter([callbacks, route, method](Request *request) {
        return request != nullptr && request->method() == method && request->url() == route
               && callbacks.isMutationAllowed(request);
    });
    server->on(route, method, [callbacks](Request *request) {
        callbacks.sendForbidden(request);
    });
}

template<typename Request, typename Server, typename Callbacks, typename Method, typename Handler>
void addBody(Server *server, const Callbacks callbacks, const char *route,
             const Method method, const Handler handler) {
    auto &entry = server->on(route, method, [callbacks](Request *request) {
        callbacks.logRequest(request);
    }, nullptr, [callbacks, handler](Request *request, const uint8_t *data, const size_t length,
                                    const size_t index, const size_t total) {
        handler(callbacks, request, data, length, index, total);
    });
    entry.setFilter([callbacks, route, method](Request *request) {
        return request != nullptr && request->method() == method && request->url() == route
               && callbacks.isMutationAllowed(request);
    });
    server->on(route, method, [callbacks](Request *request) {
        callbacks.sendForbidden(request);
    });
}

template<typename Request, typename Server, typename Callbacks, typename Method>
void configure(Server *server, const Callbacks callbacks, const Mode mode,
               const Method post, const Method put) {
    if (mode == Mode::Station) {
        addBody<Request>(server, callbacks, Routes::PUT_MODBUS_CONFIG, put, [](const Callbacks &cb, Request *request, const uint8_t *data,
                                                   const size_t length, const size_t index, const size_t total) {
            cb.handlePutModbusConfigBody(request, data, length, index, total);
        });
        addBody<Request>(server, callbacks, Routes::PUT_MQTT_CONFIG, put, [](const Callbacks &cb, Request *request, const uint8_t *data,
                                                 const size_t length, const size_t index, const size_t total) {
            cb.handlePutMqttConfigBody(request, data, length, index, total);
        });
        addBody<Request>(server, callbacks, Routes::PUT_MQTT_SECRET, post, [](const Callbacks &cb, Request *request, const uint8_t *data,
                                                  const size_t length, const size_t index, const size_t total) {
            cb.handlePutMqttSecretBody(request, data, length, index, total);
        });
        addRequest<Request>(server, callbacks, Routes::MQTT_TEST_CONNECT, post, [](const Callbacks &cb, Request *request) {
            cb.handleMqttTestConnection(request);
        });
        addRequest<Request>(server, callbacks, Routes::POST_MODBUS_EXECUTE, post, [](const Callbacks &cb, Request *request) {
            cb.handleModbusExecute(request);
        });
        addRequest<Request>(server, callbacks, Routes::POST_MBUS_DISABLE, post, [](const Callbacks &cb, Request *request) {
            cb.handleModbusDisable(request, false);
        });
        addRequest<Request>(server, callbacks, Routes::POST_MBUS_ENABLE, post, [](const Callbacks &cb, Request *request) {
            cb.handleModbusDisable(request, true);
        });
        addRequest<Request>(server, callbacks, Routes::DEVICE_RESET, post, [](const Callbacks &cb, Request *request) {
            cb.handleDeviceReset(request);
        });
    } else {
        addBody<Request>(server, callbacks, Routes::POST_WIFI_CONNECT, post, [](const Callbacks &cb, Request *request, const uint8_t *data,
                                                    const size_t length, const size_t index, const size_t total) {
            cb.handleWifiConnectBody(request, data, length, index, total);
        });
        addRequest<Request>(server, callbacks, Routes::POST_WIFI_AP_OFF, post, [](const Callbacks &cb, Request *request) {
            cb.handleWifiApOff(request);
        });
        addRequest<Request>(server, callbacks, Routes::POST_WIFI_CANCEL, post, [](const Callbacks &cb, Request *request) {
            cb.handleWifiCancel(request);
        });
    }

    addRequest<Request>(server, callbacks, Routes::POST_WIFI_RESET, post, [](const Callbacks &cb, Request *request) {
        cb.handleNetworkReset(request);
    });
}

}  // namespace MutationRouteRegistration

#endif
