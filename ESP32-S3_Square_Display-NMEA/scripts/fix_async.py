#!/usr/bin/env python3
"""Bulk convert network_setup.cpp from WebServer to AsyncWebServer API."""
import re, sys, os

path = os.path.join(os.path.dirname(__file__), "..", "src", "network_setup.cpp")

with open(path, "r") as f:
    text = f.read()

orig = text

# 1. Fix function signatures: void handle_xxx() -> void handle_xxx(AsyncWebServerRequest *request)
text = re.sub(r'^(void handle_\w+)\(\)', r'\1(AsyncWebServerRequest *request)', text, flags=re.MULTILINE)

# 2. config_server API -> async equivalents
text = text.replace("config_server.send(", "request->send(")
text = text.replace("config_server.arg(", "arg(")
text = text.replace("config_server.hasArg(", "hasArg(")
text = text.replace("config_server.method()", "request->method()")

# 3. Remove connection:close sendHeader lines
text = re.sub(r'[ \t]*config_server\.sendHeader\("Connection",\s*"close"\);\n', '', text)

# 4. Replace Location redirect pattern
text = re.sub(
    r'config_server\.sendHeader\("Location",\s*([^\n,]+),\s*true\);\s*\n\s*request->send\(302,\s*"text/plain",\s*""\);',
    r'request->redirect(\1);',
    text
)

# 5. Remove CONTENT_LENGTH_UNKNOWN lines
text = re.sub(r'[ \t]*config_server\.setContentLength\([^)]+\);\n', '', text)

# 6. Remove sendContent empty terminator lines
text = re.sub(r'[ \t]*config_server\.sendContent\(""\)[^\n]*\n', '', text)

# 7. Any remaining sendHeader -> request->sendHeader
text = text.replace("config_server.sendHeader(", "request->sendHeader(")

orig_lines = orig.splitlines()
new_lines  = text.splitlines()
changed = sum(1 for a, b in zip(orig_lines, new_lines) if a != b)
added   = len(new_lines) - len(orig_lines)
print(f"Changed {changed} lines, net line delta {added:+d}")

with open(path, "w") as f:
    f.write(text)
print("Done:", path)
