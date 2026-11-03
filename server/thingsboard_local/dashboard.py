"""Create the native ThingsBoard driver dashboard using the verified device."""
import argparse
import copy
import json
import urllib.parse
import uuid
from pathlib import Path

from .setup import PRIVATE, ROOT, ThingsBoard, load_account

TITLE = "BTL IoT · Giám sát tài xế"
WIDGET_FQN = "driver_monitor_live_v1"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--replace", action="store_true", help="Replace the generated dashboard layout, including user layout edits")
    args = parser.parse_args()
    tb = ThingsBoard()
    tb.login("tenant@thingsboard.org", load_account())
    metadata_path = PRIVATE / "dashboard.json"
    if metadata_path.exists() and not args.replace:
        meta = json.loads(metadata_path.read_text())
        try:
            tb.call("GET", "/api/dashboard/" + meta["id"])
            print("Existing dashboard:", meta["url"])
            return
        except RuntimeError as exc:
            if "HTTP 404" not in str(exc):
                raise
    device = json.loads((PRIVATE / "device.json").read_text())
    directory = Path(__file__).parent
    descriptor = {
        "type": "latest", "sizeX": 24, "sizeY": 8, "resources": [],
        "templateHtml": '<div class="dm-panel"><div class="dm-header"><span class="dm-live"></span><span class="dm-age"></span></div><div class="dm-grid"></div></div>',
        "templateCss": (directory / "dashboard_status.css").read_text(encoding="utf-8"),
        "controllerScript": (directory / "dashboard_status.js").read_text(encoding="utf-8"),
        "settingsSchema": '{"schema":{"type":"object","properties":{"securityOnly":{"type":"boolean","default":false}}},"form":["securityOnly"]}',
        "dataKeySettingsSchema": '{}',
        "defaultConfig": json.dumps({"datasources": [], "showTitle": False, "backgroundColor": "#eef3f5",
                                      "settings": {"securityOnly": False}, "padding": "0px", "actions": {}}),
    }
    try:
        widget_type = tb.call("GET", "/api/widgetType?fqn=tenant." + WIDGET_FQN)
    except RuntimeError as exc:
        if "HTTP 404" not in str(exc):
            raise
        widget_type = {"name": "Driver monitor · Verified telemetry", "fqn": WIDGET_FQN}
    widget_type.update(descriptor=descriptor, deprecated=False)
    saved_type = tb.call("POST", "/api/widgetType", widget_type)
    full_type = "tenant." + saved_type["fqn"]
    tb.call("GET", "/api/widgetType?fqn=" + urllib.parse.quote(full_type))
    alias_id = str(uuid.uuid4())
    widgets, layout = {}, {}
    keys = ["driver_present", "face_holdover", "eyes_valid", "eyes_closed", "eye_closed_duration",
            "mouth_valid", "mouth_open", "perclos", "perclos_valid", "perclos_window_ready", "perclos_observed_s",
            "blink_count", "yawn_count", "blink_window_count", "yawn_window_count", "alert_reasons",
            "alert_reasons_mask", "boot_id", "uptime_ms", "lighting_state", "face_luma", "scene_luma",
            "integrity_status", "last_verified_at", "auth_sequence", "guard_rejected_count", "guard_bad_mac_count",
            "guard_duplicate_count", "guard_replay_rejected_count"]

    def key(name, label=None, color="#168a76", post=None, settings=None, units="", decimals=0):
        item = {"name": name, "type": "timeseries", "label": label or name, "color": color,
                "settings": settings or {}, "units": units, "decimals": decimals}
        if post:
            item.update(usePostProcessing=True, postFuncBody=post)
        return item

    def datasource(data_keys):
        return [{"type": "entity", "entityAliasId": alias_id, "dataKeys": data_keys, "name": "ESP32S3 Verified"}]

    def add(fqn, kind, config, row, height):
        identity = str(uuid.uuid4())
        config.update(widgetStyle={"borderRadius": "10px"}, borderRadius="10px", enableFullscreen=True,
                      enableDataExport=True, actions={}, useDashboardTimewindow=True)
        widgets[identity] = {"id": identity, "typeFullFqn": fqn, "type": kind,
                             "sizeX": 24, "sizeY": height, "config": config}
        layout[identity] = {"sizeX": 24, "sizeY": height, "row": row, "col": 0,
                            "mobileOrder": len(layout), "mobileHeight": 18 if row == 0 else height + 1}

    def template(fqn):
        return json.loads(tb.call("GET", "/api/widgetType?fqn=" + fqn)["descriptor"]["defaultConfig"])

    status = {"datasources": datasource([key(k) for k in keys]), "showTitle": False, "padding": "0px",
              "backgroundColor": "#eef3f5", "settings": {"securityOnly": False}, "showLegend": False}
    add(full_type, "latest", status, 0, 8)
    chart = template("system.time_series_chart")
    chart.update(title="Chớp mắt, Ngáp và PERCLOS · thời gian thực", showTitle=True, backgroundColor="#ffffff")
    chart["settings"]["legendConfig"].update(position="bottom", direction="row", showAvg=False, showLatest=True)
    chart["settings"]["yAxes"]["default"].update(label="Số lần", min=0, decimals=0, units="lần")
    percent_axis = copy.deepcopy(chart["settings"]["yAxes"]["default"])
    percent_axis.update(id="perclos", order=1, label="PERCLOS (%)", min=0, max=100, position="right", units="%", decimals=1)
    chart["settings"]["yAxes"]["perclos"] = percent_axis
    line = {"type": "line", "showInLegend": True, "yAxisId": "default", "lineSettings": {
        "showLine": True, "lineWidth": 2, "smooth": False, "step": False, "showPoints": False,
        "fillAreaSettings": {"type": "none"}}}
    percent_line = copy.deepcopy(line); percent_line["yAxisId"] = "perclos"
    chart["datasources"] = datasource([
        key("blink_count", "Tổng chớp mắt", "#2196f3", settings=line, units="lần"),
        key("yawn_count", "Tổng số lần ngáp", "#f59e0b", settings=line, units="lần"),
        key("perclos", "PERCLOS", "#ed326d", post="if(value === null || value === undefined) return null; return Number(value)*100;",
            settings=percent_line, units="%", decimals=1)])
    add("system.time_series_chart", "timeseries", chart, 8, 5)
    history = template("system.time_series_chart")
    history.update(title="Lịch sử cảnh báo", showTitle=True, backgroundColor="#ffffff")
    history["settings"]["showLegend"] = False
    formatter = "var names=['Bình thường','Nhắm mắt lâu','Ngáp nhiều','PERCLOS cao','Không thấy mặt']; return names[Math.round(Number(value))] || '';"
    history["settings"]["yAxes"]["default"].update(min=0, max=4, interval=1, decimals=0, label="", ticksFormatter=formatter)
    stepped = copy.deepcopy(line); stepped["lineSettings"].update(step=True, stepType="end")
    stepped["tooltipValueFormatter"] = formatter
    history["datasources"] = datasource([key("alert_reasons_mask", "Cảnh báo", "#168a76", settings=stepped,
        post="var v=Number(value)||0; return (v&1)?1:(v&4)?2:(v&8)?3:(v&16)?4:0;")])
    add("system.time_series_chart", "timeseries", history, 13, 4)
    security = copy.deepcopy(status); security["settings"]["securityOnly"] = True
    add(full_type, "latest", security, 17, 3)
    table = template("system.cards.timeseries_table")
    table.update(title="Chi tiết mẫu đã xác minh", showTitle=True, backgroundColor="#ffffff")
    table["settings"].update(defaultPageSize=10, showTimestamp=True, sortOrder="-timestamp", displayPagination=True,
                              enableSearch=True, useRowStyleFunction=False)
    table["datasources"] = datasource([key("auth_sequence", "Sequence"), key("alert_reasons", "Cảnh báo"),
        key("integrity_status", "Xác thực"), key("blink_count", "Chớp mắt"), key("yawn_count", "Ngáp"),
        key("perclos", "PERCLOS (%)", post="if(value === null) return null; return Number(value)*100;", decimals=1)])
    add("system.cards.timeseries_table", "timeseries", table, 20, 5)
    config = {
        "widgets": widgets,
        "states": {"default": {"name": TITLE, "root": True, "layouts": {"main": {"widgets": layout,
            "gridSettings": {"columns": 24, "margin": 12, "outerMargin": True, "backgroundColor": "#eef3f5",
                             "layoutType": "default", "autoFillHeight": False, "mobileAutoFillHeight": False, "mobileRowHeight": 70}}}}},
        "entityAliases": {alias_id: {"id": alias_id, "alias": "ESP32-S3 · Dữ liệu đã xác minh",
            "filter": {"type": "singleEntity", "resolveMultiple": False,
                       "singleEntity": {"entityType": "DEVICE", "id": device["id"]}}}},
        "timewindow": {"selectedTab": 0, "realtime": {"realtimeType": 0, "interval": 5000, "timewindowMs": 3600000},
                       "aggregation": {"type": "NONE", "limit": 10000}, "timezone": "Asia/Bangkok"},
        "settings": {"stateControllerId": "entity", "showTitle": True, "showDashboardTimewindow": True,
                     "showDashboardsSelect": True, "showDashboardExport": True, "toolbarAlwaysOpen": True,
                     "hideToolbar": False, "showEntitiesSelect": False, "showFilters": False}, "filters": {},
    }
    dashboards = tb.call("GET", "/api/tenant/dashboards?pageSize=100&page=0")["data"]
    existing = next((d for d in dashboards if d["title"] == TITLE), None)
    body = {"title": TITLE, "configuration": config}
    if existing:
        body["id"] = existing["id"]
    saved = tb.call("POST", "/api/dashboard", body)
    identity = saved["id"]["id"]
    url = "http://127.0.0.1:8080/dashboards/" + identity
    metadata_path.write_text(json.dumps({"id": identity, "url": url, "title": TITLE}, ensure_ascii=False, indent=2), encoding="utf-8")
    export = PRIVATE / "dashboard-export.json"
    export.write_text(json.dumps(body, ensure_ascii=False, indent=2), encoding="utf-8")
    # Verify persistence and real data keys; no synthetic datapoints are added.
    check = tb.call("GET", "/api/dashboard/" + identity)
    assert len(check["configuration"]["widgets"]) == 5
    values = tb.call("GET", f"/api/plugins/telemetry/DEVICE/{device['id']}/values/timeseries?keys=auth_sequence,integrity_status")
    print("Native ThingsBoard dashboard created:", url)
    print("16 status cards; blink/yawn/PERCLOS chart; alert history; security counters; detail table.")
    if values.get("auth_sequence") and values.get("integrity_status"):
        print("Live sequence:", values["auth_sequence"][0]["value"], "integrity:", values["integrity_status"][0]["value"])
    else:
        print("Dashboard ready; waiting for the first verified ESP32 sample.")


if __name__ == "__main__":
    main()
