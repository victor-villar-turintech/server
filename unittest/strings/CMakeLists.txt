
INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/strings)
MY_ADD_TESTS(strings json uca_v9 LINK_LIBRARIES strings mysys)
