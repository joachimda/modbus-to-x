#!/usr/bin/env python3
"""Exercise MQTT/Modbus owner handoffs against a development device.

The script re-saves the exact configuration bytes it first reads from the
device; it does not invent credentials or change persisted configuration.
"""

import argparse
import json
import threading
import time
import urllib.error
import urllib.parse
import urllib.request


def request(base_url, path, method="GET", body=None, timeout=4.0):
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
    parser.add_argument("--duration", type=float, default=30.0)
    args = parser.parse_args()
    base_url = args.base_url.rstrip("/")

    _, modbus_config = request(base_url, "/conf/config.json")
    _, mqtt_config = request(base_url, "/conf/mqtt.json")
    failures = []
    samples = []
    lock = threading.Lock()
    stop = threading.Event()

    def record_failure(worker, error):
        with lock:
            failures.append(f"{worker}: {error}")

    def stats_worker():
        while not stop.is_set():
            try:
                _, payload = request(base_url, "/api/stats/system", timeout=2.0)
                sample = json.loads(payload)
                with lock:
                    samples.append(sample)
            except Exception as error:  # network errors are part of the result
                record_failure("stats", error)
            time.sleep(0.1)

    def configuration_worker():
        iteration = 0
        while not stop.is_set():
            path, body = (("/api/config/modbus", modbus_config)
                          if iteration % 2 == 0
                          else ("/api/config/mqtt", mqtt_config))
            try:
                request(base_url, path, method="PUT", body=body, timeout=6.0)
            except Exception as error:
                record_failure("configuration", error)
            iteration += 1
            time.sleep(0.4)

    def state_worker():
        enabled = False
        while not stop.is_set():
            path = "/api/modbus/state/enable" if enabled else "/api/modbus/state/disable"
            try:
                request(base_url, path, method="POST", body=b"", timeout=6.0)
            except Exception as error:
                record_failure("modbus-state", error)
            enabled = not enabled
            time.sleep(0.3)

    def command_worker():
        query = urllib.parse.urlencode({
            "devId": "owner-stress",
            "dpId": "owner-stress",
            "func_code": "3",
            "addr": "0",
            "len": "1",
        })
        while not stop.is_set():
            try:
                # The unattached hardware path normally reaches its five-second
                # Modbus response timeout. Keep the HTTP timeout comfortably
                # above that boundary so a completed response is not mistaken
                # for a transport failure.
                request(base_url, "/api/modbus/execute?" + query, method="POST", body=b"", timeout=12.0)
            except Exception as error:
                record_failure("modbus-command", error)
            time.sleep(2.5)

    workers = [
        threading.Thread(target=stats_worker, daemon=True),
        threading.Thread(target=configuration_worker, daemon=True),
        threading.Thread(target=state_worker, daemon=True),
        threading.Thread(target=command_worker, daemon=True),
    ]
    try:
        for worker in workers:
            worker.start()
        time.sleep(args.duration)
    finally:
        stop.set()
        for worker in workers:
            worker.join(timeout=14.0)
        # State toggles are runtime-only, so reapplying both exact snapshots is
        # required to leave the device in its original persisted/runtime state.
        put_with_retry(base_url, "/api/config/mqtt", mqtt_config)
        put_with_retry(base_url, "/api/config/modbus", modbus_config)

    # Let accepted requests that outlived a client-side timeout release their
    # owner/job slots before recording the residual heap value.
    time.sleep(5.0)
    try:
        _, payload = request(base_url, "/api/stats/system", timeout=3.0)
        samples.append(json.loads(payload))
    except Exception as error:
        record_failure("final-stats", error)

    if not samples:
        raise SystemExit("No statistics samples were received")
    first = samples[0]
    last = samples[-1]
    mqtt_connection_transitions = sum(
        bool(samples[index].get("mqttConnected")) != bool(samples[index - 1].get("mqttConnected"))
        for index in range(1, len(samples))
    )
    summary = {
        "samples": len(samples),
        "failures": failures,
        "heapFreeFirst": first.get("heapFree"),
        "heapFreeLast": last.get("heapFree"),
        "heapFreeMinimum": min(sample.get("heapFree", 0) for sample in samples),
        "heapLargestFirst": first.get("heapLargest"),
        "heapLargestLast": last.get("heapLargest"),
        "heapLargestMinimum": min(sample.get("heapLargest", 0) for sample in samples),
        "heapMinimumEver": last.get("heapMin"),
        "mqttGeneration": last.get("mqttGeneration"),
        "mqttConnectedObserved": any(sample.get("mqttConnected") for sample in samples),
        "mqttDisconnectedObserved": any(not sample.get("mqttConnected") for sample in samples),
        "mqttConnectionTransitions": mqtt_connection_transitions,
        "modbusGeneration": last.get("modbusGeneration"),
        "mqttOwnerViolations": last.get("mqttOwnerViolations"),
        "modbusOwnerViolations": last.get("modbusOwnerViolations"),
        "mqttCommandFailures": last.get("mqttErrorCount"),
        "modbusCommandFailures": last.get("modbusCommandFailures"),
        "resetObserved": any(samples[index].get("uptimeMs", 0) < samples[index - 1].get("uptimeMs", 0)
                             for index in range(1, len(samples))),
    }
    print(json.dumps(summary, indent=2, sort_keys=True))
    if failures or summary["resetObserved"] or summary["mqttOwnerViolations"] \
            or summary["modbusOwnerViolations"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
