// Handler for toggling test mode from the web UI
#include <Arduino.h>
#include <WebServer.h>
extern WebServer config_server;
extern bool test_mode;
void handle_toggle_test_mode() {
    if (config_server.method() == HTTP_POST) {
        test_mode = !test_mode;
        // Redirect back to the gauges page, preserving active tab if provided
        String redirectPath = "/gauges";
        if (config_server.hasArg("active_tab")) {
            String at = config_server.arg("active_tab");
            at.trim();
            if (at.length() > 0) redirectPath += "#tab" + at;
        }
        config_server.sendHeader("Location", redirectPath, true);
        config_server.send(302, "text/plain", "");
    } else {
        config_server.send(405, "text/plain", "Method Not Allowed");
    }
}