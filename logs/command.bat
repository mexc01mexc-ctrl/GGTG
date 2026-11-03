@echo ON
set "NoDefaultCurrentDirectoryInExePath="
set "TARGET="
set "_CL_="
set "MSYS2_ARG_CONV_EXCL=*"
if exist libwebp (attrib -r -s -h libwebp /s /d 2>nul & rmdir /Q /S libwebp 2>nul || ver >nul)
if %errorlevel% neq 0 exit /b %errorlevel%
call git clone -b v1.6.0 https://github.com/webmproject/libwebp.git
if %errorlevel% neq 0 exit /b %errorlevel%
call cd libwebp
if %errorlevel% neq 0 exit /b %errorlevel%
call nmake /f Makefile.vc CFG=debug-static OBJDIR=out RTLIBCFG=static all
if %errorlevel% neq 0 exit /b %errorlevel%
call nmake /f Makefile.vc CFG=release-static OBJDIR=out RTLIBCFG=static all
if %errorlevel% neq 0 exit /b %errorlevel%
call copy out\release-static\%X8664%\lib\libwebp.lib out\release-static\%X8664%\lib\webp.lib
if %errorlevel% neq 0 exit /b %errorlevel%
call copy out\release-static\%X8664%\lib\libwebpdemux.lib out\release-static\%X8664%\lib\webpdemux.lib
if %errorlevel% neq 0 exit /b %errorlevel%
call copy out\release-static\%X8664%\lib\libwebpmux.lib out\release-static\%X8664%\lib\webpmux.lib
if %errorlevel% neq 0 exit /b %errorlevel%
