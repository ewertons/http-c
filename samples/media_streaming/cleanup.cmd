@echo off
REM ---------------------------------------------------------------------------
REM  cleanup.cmd -- tear down everything launch_with_docker.cmd may have
REM  left behind on the host:
REM
REM    1. Stops + removes any running "http-c-media-streaming" container.
REM    2. Removes the "http-c-media-streaming:latest" image.
REM    3. Removes the cert dir under %TEMP%\http-c-media-streaming-cert.
REM    4. Removes any cert in Cert:\CurrentUser\Root whose Subject contains
REM       "O=http-c sample" (the marker the launcher mints them with).
REM       Multiple matches are removed -- so if you closed the launcher
REM       with the window X button across several runs, this cleans them
REM       all up in one go.
REM
REM  Idempotent: rerunning is safe and quietly no-ops on anything that's
REM  already gone.
REM ---------------------------------------------------------------------------

setlocal

set "IMAGE_TAG=http-c-media-streaming:latest"
set "CONTAINER_NAME=http-c-media-streaming"
set "CERT_DIR=%TEMP%\http-c-media-streaming-cert"

echo === http-c media-streaming cleanup ===
echo.

REM --- 1. Container -------------------------------------------------------
where docker >nul 2>&1
if errorlevel 1 (
    echo [skip] docker not on PATH; skipping container/image cleanup.
) else (
    for /f "delims=" %%C in ('docker ps -aq --filter "name=^%CONTAINER_NAME%$" 2^>nul') do (
        echo Stopping container %%C ...
        docker stop "%%C" >nul 2>&1
        docker rm   "%%C" >nul 2>&1
    )

    REM --- 2. Image -------------------------------------------------------
    docker image inspect "%IMAGE_TAG%" >nul 2>&1
    if errorlevel 1 (
        echo [skip] image %IMAGE_TAG% not present.
    ) else (
        echo Removing image %IMAGE_TAG% ...
        docker rmi -f "%IMAGE_TAG%" >nul 2>&1
    )
)

REM --- 3. Cert dir on disk ------------------------------------------------
if exist "%CERT_DIR%" (
    echo Removing cert dir %CERT_DIR% ...
    rmdir /s /q "%CERT_DIR%"
) else (
    echo [skip] cert dir %CERT_DIR% does not exist.
)

REM --- 4. Trusted-root entries --------------------------------------------
echo Removing any "http-c sample" certs from Cert:\CurrentUser\Root ...
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='SilentlyContinue'; $certs = Get-ChildItem Cert:\CurrentUser\Root | Where-Object { $_.Subject -match 'http-c sample' }; if ($certs) { foreach ($c in $certs) { Remove-Item -Path ('Cert:\CurrentUser\Root\' + $c.Thumbprint) -Force; Write-Host ('  removed ' + $c.Thumbprint) } } else { Write-Host '  (none found)' }"

echo.
echo === Cleanup complete ===

endlocal
