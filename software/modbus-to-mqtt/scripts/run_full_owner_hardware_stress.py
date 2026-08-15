#!/usr/bin/env python3
"""Temporarily exercise live MQTT/Modbus generations, then restore config."""

import argparse
import json
import threading
import time
import urllib.parse
import urllib.request


def request(base_url, path, method="GET", body=None, timeout=10.0):
    headers = {"X-MBX-Request": "1"}
    if body is not None:
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(base_url + path, data=body, headers=headers, method=method)
    with urllib.request.urlopen(req, timeout=timeout) as response:
        return response.status, response.read()


def put_with_retry(base_url, path, body):
    last_error = None
    for _ in range(5):
        try:
            request(base_url, path, method="PUT", body=body, timeout=12.0)
            return
        except Exception as error:
            last_error = error
            time.sleep(1.0)
    raise last_error


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", required=True)
    parser.add_argument("--broker-host", required=True)
    parser.add_argument("--duration", type=float, default=45.0)
    args = parser.parse_args()
    base_url = args.base_url.rstrip("/")
    _, original_modbus = request(base_url, "/conf/config.json")
    _, original_mqtt = request(base_url, "/conf/mqtt.json")

    mqtt_config = json.dumps({
        "enabled": True,
        "broker_ip": args.broker_host,
        "broker_url": "",
        "broker_port": "1883",
        "user": "",
        "root_topic": "issue4",
    }, separators=(",", ":")).encode()
    mqtt_disabled_config = json.dumps({
        "enabled": False,
        "broker_ip": args.broker_host,
        "broker_url": "",
        "broker_port": "1883",
        "user": "",
        "root_topic": "issue4",
    }, separators=(",", ":")).encode()
    modbus_config = json.dumps({
        "bus": {"baud": 9600, "serialFormat": "8N1", "enabled": True},
        "devices": [{
            "id": "owner-stress",
            "name": "Owner Stress",
            "slaveId": 247,
            "mqttEnabled": True,
            "homeassistantDiscoveryEnabled": True,
            "dataPoints": [
                {"id": "owner-stress.read", "name": "Read", "function": 3, "address": 0,
                 "numOfRegisters": 1, "scale": 1.0, "dataType": "uint16", "unit": "",
                 "topic": "", "registerSlice": "full", "poll_interval_ms": 5000},
                {"id": "owner-stress.write", "name": "Write", "function": 6, "address": 0,
                 "numOfRegisters": 1, "scale": 1.0, "dataType": "uint16", "unit": "",
                 "topic": "issue4/write", "registerSlice": "full", "poll_interval_ms": 0},
            ],
        }],
    }, separators=(",", ":")).encode()

    failures = []
    samples = []
    stop = threading.Event()

    def stats_worker():
        while not stop.is_set():
            try:
                _, payload = request(base_url, "/api/stats/system", timeout=4.0)
                samples.append(json.loads(payload))
            except Exception as error:
                failures.append(f"stats: {error}")
            time.sleep(0.25)

    try:
        # Install the bridge plan while the synthetic broker configuration is
        # disabled. Enabling it afterward ensures the broker, rather than a
        # plan-replacement disconnect, terminates both initial live sessions.
        put_with_retry(base_url, "/api/config/mqtt", mqtt_disabled_config)
        put_with_retry(base_url, "/api/config/modbus", modbus_config)
        put_with_retry(base_url, "/api/config/mqtt", mqtt_config)
        worker = threading.Thread(target=stats_worker, daemon=True)
        worker.start()
        started = time.monotonic()
        deadline = started + args.duration
        iteration = 0
        while time.monotonic() < deadline:
            try:
                # Leave the first two broker sessions alive long enough for the
                # broker to force their disconnects. Reconfiguration begins
                # afterward and remains slower than a no-slave Modbus timeout.
                if time.monotonic() - started >= 25.0:
                    if iteration % 2 == 0:
                        put_with_retry(base_url, "/api/config/mqtt", mqtt_config)
                    else:
                        put_with_retry(base_url, "/api/config/modbus", modbus_config)
                query = urllib.parse.urlencode({
                    "devId": "owner-stress", "dpId": "owner-stress.read",
                    "func_code": "3", "addr": "0", "len": "1",
                })
                request(base_url, "/api/modbus/execute?" + query, method="POST", body=b"", timeout=10.0)
            except Exception as error:
                failures.append(f"operation: {error}")
            iteration += 1
            time.sleep(2.5)
        stop.set()
        worker.join(timeout=6.0)
    finally:
        stop.set()
        put_with_retry(base_url, "/api/config/mqtt", original_mqtt)
        put_with_retry(base_url, "/api/config/modbus", original_modbus)

    time.sleep(6.0)
    _, final_payload = request(base_url, "/api/stats/system", timeout=5.0)
    final = json.loads(final_payload)
    summary = {
        "samples": len(samples),
        "failures": failures,
        "resetObserved": any(samples[index].get("uptimeMs", 0) < samples[index - 1].get("uptimeMs", 0)
                             for index in range(1, len(samples))),
        "mqttConnectedObserved": any(sample.get("mqttConnected") for sample in samples),
        "mqttGenerationMaximum": max((sample.get("mqttGeneration", 0) for sample in samples), default=0),
        "modbusGenerationMaximum": max((sample.get("modbusGeneration", 0) for sample in samples), default=0),
        "mqttOwnerViolations": final.get("mqttOwnerViolations"),
        "modbusOwnerViolations": final.get("modbusOwnerViolations"),
        "mqttCommandFailures": final.get("mqttErrorCount"),
        "modbusCommandFailures": final.get("modbusCommandFailures"),
        "restoredBroker": final.get("broker"),
        "restoredDevices": final.get("devices"),
        "heapFreeFinal": final.get("heapFree"),
        "heapLargestFinal": final.get("heapLargest"),
        "heapMinimum": min((sample.get("heapMin", 0) for sample in samples), default=0),
        "heapLargestMinimum": min((sample.get("heapLargest", 0) for sample in samples), default=0),
    }
    print(json.dumps(summary, indent=2, sort_keys=True))
    if failures or summary["resetObserved"] or not summary["mqttConnectedObserved"] \
            or summary["mqttOwnerViolations"] or summary["modbusOwnerViolations"] \
            or summary["mqttCommandFailures"] or summary["modbusCommandFailures"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
