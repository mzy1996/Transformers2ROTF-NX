@echo off
rem codegen wrapper: rexglue's generated add_custom_command is
rem "rex::rexglue codegen <manifest>"; this wrapper appends the two
rem project-required flags that the embedded template omits.
rem   --update_data_root=..\update  apply the TU patch (default.xexp)
rem   --max_file_size_bytes=4194304 allow >1 MB functions (default 1 MB kills
rem                               the build on sub_82B7E3A0, 1.36 MB)
rem The wrapper is invoked with WORKING_DIRECTORY = <project>/recomp, so both
rem relative paths resolve there.
"%~dp0..\repos\rexglue-release\bin\rexglue.exe" %* --update_data_root=..\update --max_file_size_bytes=4194304
if errorlevel 1 exit /b %ERRORLEVEL%
rem The prebuilt rexglue.exe embeds an old pch template whose only non-Apple
rem branch is `__attribute__((alias(...)))`; GCC 15 ignores aliases in
rem declaration position, so every codegen run rewrites T2ROTF_pch.h into a
rem form that links with thousands of undefined `sub_*` symbols. Restore the
rem GCC weak-thunk variant (same shape as the nfsmw-nx SDK template) so the
rem build keeps producing real `sub_*` symbols.
copy /Y "%~dp0codegen_fix\pch_switch_gcc_fix.h" "%~dp0generated\default\T2ROTF_pch.h" >nul
exit /b %ERRORLEVEL%