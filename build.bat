@echo off
chcp 65001 >nul
rem  ============================================================
rem    Edraft —— 极简白板 构建脚本
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

rem 第一步：把图标资源编译成目标文件（没有 ico 就跳过，编译出无图标版本）
set RES=
if not exist "Edraft.ico" (
    echo [提示] 未找到 Edraft.ico，将编译【无图标】版本
    echo        可用 tools\png2ico\png2ico.exe Edraft.png Edraft.ico 生成
) else (
    "%RC%" app.rc -O coff -o app_res.o
    if errorlevel 1 (
        echo [错误] 图标资源编译失败
        pause
        exit /b 1
    )
    set RES=app_res.o
)

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
echo [构建成功] Edraft.exe（已嵌入图标）
for %%F in (Edraft.exe) do echo 文件大小: %%~zF 字节
pause
