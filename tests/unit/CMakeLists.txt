# 单元测试：验证最小库能正确编译、链接并通过 CTest 运行。
add_executable(smoke_test smoke_test.cpp)
target_link_libraries(smoke_test PRIVATE voxorchestra::common)
add_test(NAME smoke_test COMMAND smoke_test)
