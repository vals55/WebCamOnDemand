WebCam on demand - build notes
1. Open folder in PlatformIO (VS Code), env: esp32cam.
2. Delete sdkconfig.esp32cam if it exists (so sdkconfig.defaults is applied).
3. Build / Upload. First run: captive portal WebCam-xxxxxx, 192.168.4.1.
Pins: IO0 button (ext0), GPIO13 PIR (ext1, active high), SD 1-bit.
Code style: K&R, opening brace on the same line.
