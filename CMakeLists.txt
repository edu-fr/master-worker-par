cmake_minimum_required(VERSION 3.20)
project(mestreTrabalhador C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -lpthread -O3")

add_executable(sum sum.c)
add_executable(par_sum par_sum.c)
