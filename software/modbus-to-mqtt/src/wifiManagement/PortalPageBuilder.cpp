#include "wifiManagement/PortalPageBuilder.h"

auto PortalPageBuilder::buildRoot(const char * apName) -> String {

    String page = FPSTR(WFM_HTTP_HEAD);
    page.replace("{v}", "Options");
    page += FPSTR(HTTP_SCRIPT);
    page += FPSTR(HTTP_STYLE);
    page += FPSTR(HTTP_HEAD_END);
    page += "<h1>";
    page += *apName;
    page += "</h1>";
    page += F("<h3>WiFi Management Portal</h3>");
    page += FPSTR(HTTP_PORTAL_OPTIONS);
    page += FPSTR(HTTP_END);
    return page;
}