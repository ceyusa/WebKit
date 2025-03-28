set(WTF_PLATFORM_WIN_CAIRO 1)

include(OptionsWin)

# TODO: Move this above OptionsWin when WebKitLegacy is deprecated
set(ENABLE_WEBKIT ON)

find_package(Cairo 1.15.12 REQUIRED)
find_package(CURL 7.77.0 REQUIRED)
find_package(ICU 61.2 REQUIRED COMPONENTS data i18n uc)
find_package(JPEG 1.5.2 REQUIRED)
find_package(LibXml2 2.9.7 REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PNG 1.6.34 REQUIRED)
find_package(SQLite3 3.23.1 REQUIRED)
find_package(ZLIB 1.2.11 REQUIRED)
find_package(LibPSL 0.20.2 REQUIRED)

if (ENABLE_XSLT)
    find_package(LibXslt 1.1.32 REQUIRED)
endif ()

# Optional packages
find_package(LCMS2)
if (LCMS2_FOUND)
    SET_AND_EXPOSE_TO_BUILD(USE_LCMS ON)
endif ()

find_package(JPEGXL)
if (JPEGXL_FOUND)
    SET_AND_EXPOSE_TO_BUILD(USE_JPEGXL ON)
endif ()

find_package(OpenJPEG 2.3.1)
if (OpenJPEG_FOUND)
    SET_AND_EXPOSE_TO_BUILD(USE_OPENJPEG ON)
endif ()

find_package(WOFF2 1.0.2 COMPONENTS dec)
if (WOFF2_FOUND)
    SET_AND_EXPOSE_TO_BUILD(USE_WOFF2 ON)
endif ()

find_package(WebP COMPONENTS demux)
if (WebP_FOUND)
    SET_AND_EXPOSE_TO_BUILD(USE_WEBP ON)
endif ()

set(USE_ANGLE_EGL ON)

SET_AND_EXPOSE_TO_BUILD(USE_ANGLE ON)
SET_AND_EXPOSE_TO_BUILD(USE_CAIRO ON)
SET_AND_EXPOSE_TO_BUILD(USE_CURL ON)
SET_AND_EXPOSE_TO_BUILD(USE_GRAPHICS_LAYER_TEXTURE_MAPPER ON)
SET_AND_EXPOSE_TO_BUILD(USE_GRAPHICS_LAYER_WC ON)
SET_AND_EXPOSE_TO_BUILD(USE_EGL ON)
SET_AND_EXPOSE_TO_BUILD(USE_OPENGL_ES ON)
SET_AND_EXPOSE_TO_BUILD(HAVE_OPENGL_ES_3 ON)
SET_AND_EXPOSE_TO_BUILD(USE_OPENSSL ON)
SET_AND_EXPOSE_TO_BUILD(USE_TEXTURE_MAPPER ON)
SET_AND_EXPOSE_TO_BUILD(USE_TEXTURE_MAPPER_GL ON)
SET_AND_EXPOSE_TO_BUILD(USE_MEDIA_FOUNDATION ${ENABLE_VIDEO})
SET_AND_EXPOSE_TO_BUILD(USE_INSPECTOR_SOCKET_SERVER ${ENABLE_REMOTE_INSPECTOR})

SET_AND_EXPOSE_TO_BUILD(ENABLE_DEVELOPER_MODE ${DEVELOPER_MODE})

SET_AND_EXPOSE_TO_BUILD(HAVE_OS_DARK_MODE_SUPPORT 1)

# See if OpenSSL implementation is BoringSSL
cmake_push_check_state()
set(CMAKE_REQUIRED_INCLUDES "${OPENSSL_INCLUDE_DIR}")
set(CMAKE_REQUIRED_LIBRARIES "${OPENSSL_LIBRARIES}")
WEBKIT_CHECK_HAVE_SYMBOL(USE_BORINGSSL OPENSSL_IS_BORINGSSL openssl/ssl.h)
cmake_pop_check_state()

# CoreFoundation is required when building WebKitLegacy
if (ENABLE_WEBKIT_LEGACY)
    SET_AND_EXPOSE_TO_BUILD(USE_CF ON)
    set(COREFOUNDATION_LIBRARY CFlite)
endif ()

add_definitions(-DWTF_PLATFORM_WIN_CAIRO=1)
add_definitions(-DNOCRYPT)

# Override library types
set(WebCore_LIBRARY_TYPE OBJECT)
set(WebCoreTestSupport_LIBRARY_TYPE OBJECT)
