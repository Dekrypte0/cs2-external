#pragma once
#include <fstream>
#include <iostream>
#include <string>

inline void Log(const std::string& msg) {
    /* identical consecutive lines carry no information (the wait loop and
     * steady-state diagnostics repeat by nature) */
    static std::string lastMsg;
    if (msg == lastMsg) return;
    lastMsg = msg;

    /* rotate once per process: diagnostics flush every line, so a long
     * session can grow this into tens of megabytes. 4 MB is plenty of story. */
    static bool prepared = false;
    if (!prepared) {
        prepared = true;
        std::ifstream probe("helper_debug.log", std::ios::ate | std::ios::binary);
        if (probe.is_open() && probe.tellg() > (std::streamoff)(4 * 1024 * 1024)) {
            probe.close();
            std::ofstream trunc("helper_debug.log", std::ios::trunc);
            trunc << "[*] log rotated (previous run exceeded 4 MB)\n";
        }
    }

    static std::ofstream logFile("helper_debug.log", std::ios::app);
    if (logFile.is_open()) {
        logFile << msg << '\n';
        logFile.flush();   // unbuffered diagnostics — abrupt death keeps its story
    }
    std::cout << msg << std::endl;   // live terminal feed
}
