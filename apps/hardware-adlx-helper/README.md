# InferDeck ADLX Helper

Windows C++ telemetry helper for InferDeck.

Build on the Windows GPU host:

```powershell
cmake -S apps/hardware-adlx-helper -B build/adlx-helper -DADLX_SDK_DIR=C:\path\to\ADLX
cmake --build build/adlx-helper --config Release
copy build\adlx-helper\Release\inferdeck-adlx-helper.exe bin\inferdeck-adlx-helper.exe
```

If built without `ADLX_SDK_DIR`, the helper returns a valid unavailable JSON response. The gateway treats that as a real provider failure and the dashboard shows telemetry as unavailable instead of fake metrics.
