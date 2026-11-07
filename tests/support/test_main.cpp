#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <QApplication>

QApplication& slopkit_test_application()
{
    static int          argc      = 1;
    static char         program[] = "slopkit_tests";
    static char*        argv[]    = {program, nullptr};
    static QApplication instance(argc, argv);
    return instance;
}
