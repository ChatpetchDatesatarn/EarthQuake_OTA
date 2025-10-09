@echo off
setlocal enabledelayedexpansion
REM ==== ตั้งค่าเวอร์ชันที่กำลังปล่อย ====
set FW=2.1.1

REM ==== โฟลเดอร์หลัก ====
set ROOT=C:\PlatformIO\Projects\EarthQuake_C3-v.3
set DIST=%ROOT%\dist
set OTA=%ROOT%\ota

REM ==== รายชื่อ env ทั้งหมดที่ต้องการ ====
set ENVS=wifi_gateway mesh_gateway sender_node_1 sender_node_2 sender_node_3 sender_node_4 sender_node_5 sender_node_6

echo [*] Prepare OTA folder: %OTA%
if not exist "%OTA%" mkdir "%OTA%"

REM (ล้าง .bin/.sha256 เก่าออก แต่คง manifest.json เดิมไว้ก่อน)
pushd "%OTA%"
del /q *.bin 2>nul
del /q *.sha256 2>nul
popd

REM ==== ฟังก์ชัน Copy หนึ่ง env (ลอง 2 แพทเทิร์นชื่อไฟล์) ====
REM แพทเทิร์น A:   <env>_v2.1.1.bin
REM แพทเทิร์น B:   <env>_v_2.1.1_.bin   (กรณี build เดิมมีขีดล่าง)
REM ผลลัพธ์ใน ota/ จะรีเนมให้เป็นแบบสวย A เสมอ

for %%E in (%ENVS%) do (
  set SRC_A=%DIST%\%%E\%%E_v%FW%.bin
  set SRC_B=%DIST%\%%E\%%E_v_%FW%_.bin
  set DST=%OTA%\%%E_v%FW%.bin
  set DST_SHA=%OTA%\%%E_v%FW%.bin.sha256

  echo --------------------------------------------
  echo [*] %%E

  if exist "!SRC_A!" (
    copy /y "!SRC_A!" "!DST!" >nul
    if exist "!SRC_A!.sha256" (copy /y "!SRC_A!.sha256" "!DST_SHA!" >nul)
  ) else if exist "!SRC_B!" (
    copy /y "!SRC_B!" "!DST!" >nul
    if exist "!SRC_B!.sha256" (copy /y "!SRC_B!.sha256" "!DST_SHA!" >nul)
  ) else (
    echo [!] not found in dist: ^"!SRC_A!^" or ^"!SRC_B!^"
    echo [!] skip this env
    set HASH_%%E=
    goto :continue_env
  )

  REM ==== ถ้ายังไม่มีไฟล์ .sha256 ให้สร้างใหม่ ====
  if not exist "!DST_SHA!" (
    for /f "skip=1 tokens=1 delims= " %%H in ('certutil -hashfile "!DST!" SHA256 ^| findstr /R "^[0-9A-Fa-f][0-9A-Fa-f]*$"') do (
      echo %%H> "!DST_SHA!"
      goto :got_hash_%%E
    )
    :got_hash_%%E
  )

  REM ==== อ่านค่า hash จากไฟล์ .sha256 ใส่ตัวแปร ====
  for /f "usebackq tokens=1" %%H in ("!DST_SHA!") do (
    set HASH_%%E=%%H
  )
  echo     bin: !DST!
  echo     sha: !HASH_%%E!

  :continue_env
)

REM ==== เขียน ota/manifest.json ใหม่ ====
set MF=%OTA%\manifest.json
echo {>"%MF%"
echo   "version": "%FW%",>>"%MF%"
echo   "date": "2025-10-06",>>"%MF%"
echo   "assets": {>>"%MF%"

set first=1
for %%E in (%ENVS%) do (
  set COMMA=
  if "!first!"=="0" set COMMA=,
  if "!first!"=="1" set first=0

  if defined HASH_%%E (
    echo     !COMMA!"%%E": "https://raw.githubusercontent.com/ChatpetchDatesatarn/EarthQuake_OTA/main/ota/%%E_v%FW%.bin">>"%MF%"
  ) else (
    REM ถ้าไม่มีไฟล์ ไม่เขียน assets แถวนี้
  )
)

echo   },>>"%MF%"
echo   "sha256": {>>"%MF%"

set first=1
for %%E in (%ENVS%) do (
  if defined HASH_%%E (
    set COMMA=
    if "!first!"=="0" set COMMA=,
    if "!first!"=="1" set first=0
    echo     !COMMA!"%%E": "!HASH_%%E!">>"%MF%"
  )
)

echo   }>>"%MF%"
echo }>>"%MF%"

echo --------------------------------------------
echo [*] Done. OTA folder refreshed.
echo [*] Files in OTA:
dir "%OTA%"
echo --------------------------------------------
echo [*] manifest.json preview:
type "%MF%"
echo --------------------------------------------
pause
endlocal
