@echo off
chcp 65001 >nul
rem  ============================================================
rem    Edraft 1.1.0.0 —— 极简电子草稿纸 构建脚本
rem    纯 Win32 API + GDI+，零第三方依赖
rem    按需修改下面的 MinGW 路径（g++.exe 与 windres.exe 都在其 bin 下）
rem  ============================================================

set GXX=D:\mingw64\bin\g++.exe
set RC=D:\mingw64\bin\windres.exe

if not exist "%GXX%" (
    echo [错误] 未找到 g++，请修改本文件中的 GXX 路径
    pause
    exit /b 1
)

rem 第一步：编译资源（图标 + exe 版本信息）
rem   --codepage=65001 不能省：app.rc 是 UTF-8，默认按系统 ANSI(GBK) 读会把中文版本信息解码成乱码
set RES=
set RCSRC=app.rc
if not exist "Edraft.ico" (
    echo [提示] 未找到 Edraft.ico，将编译【无图标】版本（版本信息仍会写入）
    echo        可用 tools\png2ico\png2ico.exe Edraft.png Edraft.ico 生成
    rem 把 ICON 那一行滤掉，其余（VERSIONINFO）照编
    findstr /v /c:"1 ICON" app.rc > app_ver_only.rc
    set RCSRC=app_ver_only.rc
)
"%RC%" --codepage=65001 %RCSRC% -O coff -o app_res.o
if errorlevel 1 (
    echo [错误] 资源编译失败
    pause
    exit /b 1
)
set RES=app_res.o

rem 第二步：编译并链接
"%GXX%" -std=c++17 -O2 -Wall -mwindows -o Edraft.exe ^
    main.cpp model.cpp render.cpp io.cpp ui.cpp %RES% ^
    -lgdiplus -lgdi32 -luser32 -lole32 -lcomdlg32 -lshlwapi ^
    -static-libgcc -static-libstdc++ -s

if errorlevel 1 (
    echo.
    echo [构建失败]
    pause
    exit /b 1
)

echo.
echo [构建成功] Edraft.exe（已嵌入图标与版本信息）
for %%F in (Edraft.exe) do echo 文件大小: %%~zF 字节
echo 版本信息可在 资源管理器 - 属性 - 详细信息 里查看
pause
