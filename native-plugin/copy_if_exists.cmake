# cmake -DSRC=<file> -DDST=<folder> -P copy_if_exists.cmake: copies SRC into DST when it exists
# (the native renderer plugin is only there after tools\build_native.bat).
if(EXISTS "${SRC}")
    file(COPY "${SRC}" DESTINATION "${DST}")
endif()
