cmake_minimum_required(VERSION 3.20)
project(HanabiAlarm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Vulkan REQUIRED)

# ─── Compiler options ────────────────────────────────────────────────────────
if(MSVC)
	message(STATUS "Compiler: MSVC")
	add_compile_options(/wd4819 /wd4244 /wd4267 /wd4305 "/Zc:__cplusplus")
	set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
	set(CMAKE_CXX_FLAGS_DEBUG   "/MDd /Zi /Od")
	set(CMAKE_CXX_FLAGS_RELEASE "/MD /O2")
else()
	message(STATUS "Compiler: MinGW / Clang")
	add_compile_options(-Wall -Wextra)
	set(CMAKE_CXX_FLAGS_DEBUG   "-g")
	set(CMAKE_CXX_FLAGS_RELEASE "-O3")
endif()

# ─── 3rd-party ───────────────────────────────────────────────────────────────
set(THIRD_DIR ${PROJECT_SOURCE_DIR}/3rdparty)
add_subdirectory(${THIRD_DIR})

# ─── Sources ─────────────────────────────────────────────────────────────────
file(GLOB_RECURSE MSN_SOURCES "src/*.cpp")

add_executable(${PROJECT_NAME} ${MSN_SOURCES})

# Release → no console window; Debug → console subsystem (for stdout)
# MSVC: /ENTRY:mainCRTStartup lets us keep int main() with WINDOWS subsystem
if(MSVC)
	set_target_properties(${PROJECT_NAME} PROPERTIES
		WIN32_EXECUTABLE             $<CONFIG:Release>
		LINK_FLAGS_RELEASE           "/ENTRY:mainCRTStartup"
	)
else()
	set_target_properties(${PROJECT_NAME} PROPERTIES
		WIN32_EXECUTABLE $<CONFIG:Release>
	)
endif()

# ─── Include dirs ────────────────────────────────────────────────────────────
target_include_directories(${PROJECT_NAME} PRIVATE
	${PROJECT_SOURCE_DIR}/include
	${THIRD_DIR}/imgui
	${THIRD_DIR}/imgui/backends
	${THIRD_DIR}/json/single_include
	${Vulkan_INCLUDE_DIRS}
)

# ─── Link ────────────────────────────────────────────────────────────────────
target_link_libraries(${PROJECT_NAME} PRIVATE
	IMGUI_LIB
	${Vulkan_LIBRARIES}
	comdlg32
	ole32
	taskschd
)

# ─── Copy data/ to build dir ─────────────────────────────────────────────────
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_directory
		${PROJECT_SOURCE_DIR}/data
		$<TARGET_FILE_DIR:${PROJECT_NAME}>/data
	COMMENT "Copying data/ to build output"
)
