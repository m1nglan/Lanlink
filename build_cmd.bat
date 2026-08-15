@echo off
setlocal
set MSYSTEM=
set IDF_PATH=D:\esp\.espressif\v6.0.2\esp-idf
set IDF_TOOLS_PATH=D:\esp\.espressif
set PATH=D:\esp\.espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin;D:\esp\.espressif\tools\cmake\4.0.3\bin;D:\esp\.espressif\tools\ninja\1.12.1;D:\esp\.espressif\python_env\idf6.0_py3.12_env\Scripts;%PATH%
D:\esp\.espressif\python_env\idf6.0_py3.12_env\Scripts\python.exe D:\esp\.espressif\v6.0.2\esp-idf\tools\idf.py build
endlocal
