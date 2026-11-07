include_guard(GLOBAL)

option(BW_METAL_OFFLINE_SHADERS
    "Compile and embed the project-owned stable Metal shaders when the toolchain is available"
    ON)

find_program(BW_METAL_XCRUN NAMES xcrun)

function(bw_prepare_metal_shaders output_variable source output_directory)
    if(NOT APPLE)
        message(FATAL_ERROR "bw_prepare_metal_shaders is available only on macOS")
    endif()

    set(generated "${output_directory}/bw_metal_shader_data.inc")
    set(default_air "${output_directory}/bw_metal_shader_default.air")
    set(precise_air "${output_directory}/bw_metal_shader_precise.air")
    set(default_library "${output_directory}/bw_metal_shader_default.metallib")
    set(precise_library "${output_directory}/bw_metal_shader_precise.metallib")
    set(embed_script "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/embed_metal_shader.py")
    set(embed_arguments
        --source "${source}"
        --output "${generated}")
    set(library_outputs)

    set(metal_available FALSE)
    if(BW_METAL_OFFLINE_SHADERS AND BW_METAL_XCRUN)
        execute_process(
            COMMAND "${BW_METAL_XCRUN}" --find metal
            RESULT_VARIABLE metal_result
            OUTPUT_VARIABLE metal_executable
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        execute_process(
            COMMAND "${BW_METAL_XCRUN}" --find metallib
            RESULT_VARIABLE metallib_result
            OUTPUT_VARIABLE metallib_executable
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(metal_result EQUAL 0 AND metallib_result EQUAL 0)
            set(metal_available TRUE)
        endif()
    endif()

    if(metal_available)
        set(deployment_arguments)
        if(CMAKE_OSX_DEPLOYMENT_TARGET)
            list(APPEND deployment_arguments
                "-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
        endif()
        add_custom_command(
            OUTPUT
                "${default_air}" "${precise_air}"
                "${default_library}" "${precise_library}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${output_directory}"
            COMMAND "${metal_executable}" -c "${source}" -o "${default_air}"
                    ${deployment_arguments}
            COMMAND "${metal_executable}" -c "${source}" -o "${precise_air}"
                    -fno-fast-math ${deployment_arguments}
            COMMAND "${metallib_executable}" "${default_air}" -o "${default_library}"
            COMMAND "${metallib_executable}" "${precise_air}" -o "${precise_library}"
            DEPENDS "${source}"
            COMMENT "Compiling stable Metal shader libraries"
            VERBATIM)
        list(APPEND embed_arguments
            --default-library "${default_library}"
            --precise-library "${precise_library}")
        list(APPEND library_outputs "${default_library}" "${precise_library}")
        message(STATUS "Stable Metal shaders will be compiled offline")
    elseif(BW_METAL_OFFLINE_SHADERS)
        message(WARNING
            "Apple Metal compiler tools are unavailable; this build will retain "
            "the explicit runtime source-compilation fallback")
    else()
        message(STATUS
            "Offline Metal shader compilation is disabled; runtime source fallback retained")
    endif()

    add_custom_command(
        OUTPUT "${generated}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${output_directory}"
        COMMAND "${Python3_EXECUTABLE}" "${embed_script}" ${embed_arguments}
        DEPENDS "${source}" "${embed_script}" ${library_outputs}
        COMMENT "Embedding stable Metal shader data"
        VERBATIM)
    set(${output_variable} "${generated}" PARENT_SCOPE)
endfunction()

function(bw_attach_metal_shaders target backend_source generated)
    get_filename_component(generated_directory "${generated}" DIRECTORY)
    target_sources(${target} PRIVATE "${generated}")
    target_include_directories(${target} PRIVATE "${generated_directory}")
    set_source_files_properties("${generated}" PROPERTIES HEADER_FILE_ONLY TRUE)
    set_source_files_properties("${backend_source}" PROPERTIES
        OBJECT_DEPENDS "${generated}")
endfunction()
