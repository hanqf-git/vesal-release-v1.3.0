/*
 * Copyright (c) 2023 ByteDance Inc.
 *
 * This file is part of veSAL.
 *
 * veSAL is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * veSAL is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with veSAL. If not, see <https://www.gnu.org/licenses/>.
 */

#include <cstring>
#include <iostream>

#include "quickassist/include/cpa.h"
#include "quickassist/include/dc/cpa_dc.h"
extern "C" {
#include "quickassist/lookaside/access_layer/include/icp_sal_user.h"
}
// Running this test requires real libqat_s.so and QAT env enabled.

int RegisterQatDcService(const char* name);
int GetDcInstanceNum();
int StopQatDcService();

#define IF_NOT_SUCCESS_CERR_AND_RETURN(_r, _ret) \
    if (_r != CPA_STATUS_SUCCESS) {              \
        std::cerr << "_r=" << _r << std::endl;   \
        return _ret;                             \
    }

int RegisterQatDcService(const char* name) {
    CpaStatus r = icp_sal_userStart(name);
    IF_NOT_SUCCESS_CERR_AND_RETURN(r, -1);
    return 0;
}

int GetDcInstanceNum() {
    Cpa16U num;
    CpaStatus r = cpaDcGetNumInstances(&num);
    IF_NOT_SUCCESS_CERR_AND_RETURN(r, -1);
    return num;
}

int StopQatDcService() {
    CpaStatus r = icp_sal_userStop();
    IF_NOT_SUCCESS_CERR_AND_RETURN(r, -1);
    return 0;
}

int main(int argc, char** argv) {
    // if user not specified QAT section name, try "SSL0"
    char section_name[128] = "SSL0";
    if (argc == 2) {
        strcpy(section_name, argv[1]);
    } else if (argc > 2) {
        std::cerr << "Usage: ./test [qat_section_name]\n";
        return -1;
    }
    RegisterQatDcService(section_name);
    int num = GetDcInstanceNum();
    std::cout << "Found qat instance num=" << num << ", with section_name=" << section_name
              << std::endl;
    StopQatDcService();
    return 0;
}
