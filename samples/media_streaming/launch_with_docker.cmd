@echo off
REM ---------------------------------------------------------------------------
REM  launch_with_docker.cmd -- one-shot launcher for the http-c media-
REM  streaming sample, with HTTPS that just works in Edge / Chrome.
REM
REM  What it does:
REM    1. Builds the Ubuntu 24.04 image (cached after the first run).
REM    2. Generates a stable self-signed TLS cert in
REM       %TEMP%\http-c-media-streaming-cert (one-time; reused on later
REM       runs until it expires). The cert is minted with the extensions
REM       Chromium-based browsers require for a self-signed cert in a
REM       Trusted Root store: basicConstraints=critical,CA:TRUE; keyUsage
REM       digitalSignature+keyEncipherment+keyCertSign; EKU=serverAuth;
REM       SAN=DNS:localhost,IP:127.0.0.1.
REM    3. Imports the cert into the *current user's* Trusted Root store
REM       (Cert:\CurrentUser\Root). No admin rights required.
REM    4. Runs the container with the cert dir bind-mounted at /certs.
REM       The container's entrypoint refuses to start unless those files
REM       are there, so the server inside loads the *exact* cert the host
REM       just imported into its trust store.
REM    5. On exit (Ctrl+C, or the container stopping for any reason),
REM       removes the cert from the user's Trusted Root store.
REM
REM  Notes:
REM    - Firefox uses its own cert store and will still warn. Use Edge or
REM      Chrome for the seamless experience, or accept the warning once.
REM    - If you forcibly close this window (X button) instead of Ctrl+C,
REM      cleanup is skipped. Run cleanup.cmd to remove leftover state.
REM ---------------------------------------------------------------------------

setlocal

REM Resolve the repo root (this script lives at samples\media_streaming\).
set "SCRIPT_DIR=%~dp0"
pushd "%SCRIPT_DIR%..\.."
set "REPO_ROOT=%CD%"
popd

set "IMAGE_TAG=http-c-media-streaming:latest"
set "DOCKER_DIR=%SCRIPT_DIR%docker"
set "CERT_DIR=%TEMP%\http-c-media-streaming-cert"
set "CERT_FILE=%CERT_DIR%\server.cert.pem"
set "KEY_FILE=%CERT_DIR%\server.key.pem"

echo === http-c media-streaming launcher ===
echo Repo root : %REPO_ROOT%
echo Image     : %IMAGE_TAG%
echo Cert dir  : %CERT_DIR%
echo.

where docker >nul 2>&1
if errorlevel 1 (
    echo [error] Docker is not on PATH. Install Docker Desktop and retry.
    exit /b 1
)

echo === Building image (cached after first run) ===
docker build -t "%IMAGE_TAG%" "%DOCKER_DIR%"
if errorlevel 1 (
    echo [error] docker build failed.
    exit /b 1
)

if not exist "%CERT_DIR%" mkdir "%CERT_DIR%"

REM --- Generate cert pair via a throwaway openssl run inside the image -----
REM    Using the image's openssl avoids any host-side dependencies. The
REM    bind-mount at /out is the host's %CERT_DIR%, so the freshly
REM    generated cert/key land directly on the host filesystem.
if not exist "%CERT_FILE%" (
    echo === Generating self-signed cert ===
    docker run --rm -v "%CERT_DIR%:/out" --entrypoint bash "%IMAGE_TAG%" -c "openssl req -x509 -newkey rsa:2048 -nodes -days 30 -sha256 -subj '/CN=localhost/O=http-c sample' -addext 'subjectAltName=DNS:localhost,IP:127.0.0.1' -addext 'basicConstraints=critical,CA:TRUE' -addext 'keyUsage=critical,digitalSignature,keyEncipherment,keyCertSign' -addext 'extendedKeyUsage=serverAuth' -keyout /out/server.key.pem -out /out/server.cert.pem"
    if errorlevel 1 (
        echo [error] Cert generation failed.
        exit /b 1
    )
)

if not exist "%CERT_FILE%" (
    echo [error] Cert generation reported success but %CERT_FILE% is missing.
    exit /b 1
)
if not exist "%KEY_FILE%" (
    echo [error] Cert generation reported success but %KEY_FILE% is missing.
    exit /b 1
)

REM --- Import the cert into Cert:\CurrentUser\Root and capture thumbprint -
REM    NB: Import-Certificate is finicky with PEM input on some PowerShell
REM    builds (it expects DER / .cer and silently no-ops on PEM). Using
REM    X509Certificate2 + X509Store directly works for both encodings.
REM    No admin rights are required for the CurrentUser store.
echo === Trusting cert for this user ===
set "CERT_THUMBPRINT="
for /f "usebackq delims=" %%T in (`powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; try { $cert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2; $cert.Import('%CERT_FILE%'); $store = New-Object System.Security.Cryptography.X509Certificates.X509Store('Root','CurrentUser'); $store.Open('ReadWrite'); $store.Add($cert); $store.Close(); Write-Output $cert.Thumbprint } catch { [Console]::Error.WriteLine($_.Exception.Message); exit 1 }"`) do set "CERT_THUMBPRINT=%%T"

if not defined CERT_THUMBPRINT (
    echo [warn] Could not import the cert into Cert:\CurrentUser\Root.
    echo        The browser will warn about TLS. Carrying on anyway.
) else (
    echo Imported cert thumbprint: %CERT_THUMBPRINT%
    echo Verifying it is in the store ...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "if (Test-Path 'Cert:\CurrentUser\Root\%CERT_THUMBPRINT%') { Write-Host '  OK: present in Cert:\CurrentUser\Root' } else { Write-Host '  [warn] not present after import (?)' }"
)

echo.
echo === Running container ===
echo Open https://localhost:8086/ in Edge or Chrome.
echo (Press Ctrl+C in this window to stop and untrust the cert.)
echo.

REM    Bind-mount the host cert dir at /certs. The entrypoint inside the
REM    container reads $CERT_DIR (default /certs) and refuses to start
REM    unless server.cert.pem + server.key.pem are present there. So the
REM    server is GUARANTEED to use the same cert we just imported into
REM    the host's trust store.
docker run --rm -it ^
    --name http-c-media-streaming ^
    -p 8086:8086 ^
    -v "%REPO_ROOT%:/src:ro" ^
    -v "%CERT_DIR%:/certs:ro" ^
    -e CERT_DIR=/certs ^
    "%IMAGE_TAG%"

echo.
echo === Untrusting cert ===
if defined CERT_THUMBPRINT (
    powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='SilentlyContinue'; Remove-Item -Path 'Cert:\CurrentUser\Root\%CERT_THUMBPRINT%' -Force; Write-Host ('Removed cert ' + '%CERT_THUMBPRINT%' + ' from Cert:\CurrentUser\Root')"
)

endlocal
