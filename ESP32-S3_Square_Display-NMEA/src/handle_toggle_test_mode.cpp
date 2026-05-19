// Handler for toggling test mode from the web UI
#include <Arduino.h>
#include <WebServer.h>
extern WebServer config_server;
extern bool test_mode;
void handle_toggle_test_mode() {
    // Registered for HTTP_POST only.
    test_mode = !test_mode;
    // Return JSON for AJAX caller (no redirect / page reload)
    config_server.sendHeader("Connection", "close");
    config_server.send(200, "application/json",
        String("{\"ok\":true,\"test_mode\":") + (test_mode ? "true" : "false") + "}");
}