add_executable(test_core test_core.cpp)
target_link_libraries(test_core PRIVATE framereview_core Qt6::Test)
add_test(NAME test_core COMMAND test_core)

add_executable(test_runtime test_runtime.cpp)
target_link_libraries(test_runtime PRIVATE framereview_core Qt6::Test)
add_test(NAME test_runtime COMMAND test_runtime)

add_executable(test_features test_features.cpp)
target_link_libraries(test_features PRIVATE framereview_core Qt6::Test)
add_test(NAME test_features COMMAND test_features)

# Needs a window system with OpenGL; the tests skip themselves when no context can be created.
add_executable(test_ui test_ui.cpp)
target_link_libraries(test_ui PRIVATE framereview_ui Qt6::Test)
add_test(NAME test_ui COMMAND test_ui)
