# CMake initial cache file for Anvil
SET (CMAKE_Fortran_COMPILER mpif90 CACHE FILEPATH "")
SET (CMAKE_C_COMPILER mpicc CACHE FILEPATH "")
SET (CMAKE_CXX_COMPILER mpicxx CACHE FILEPATH "")

SET (NETCDF_DIR $ENV{NETCDF_PATH} CACHE FILEPATH "")
SET (PNETCDF_DIR $ENV{PNETCDF_PATH} CACHE FILEPATH "")
EXECUTE_PROCESS(COMMAND ${NETCDF_DIR}/bin/nf-config --flibs
  RESULT_VARIABLE NFCONFIG_RESULT
  OUTPUT_VARIABLE NFCONFIG_OUTPUT
  ERROR_VARIABLE  NFCONFIG_ERROR
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
IF (${NFCONFIG_ERROR})
  MESSAGE(WARNING "${NETCDF_DIR}/bin/nf-config --flibs produced an error. Default linking will be used.")
ELSE ()
  SET (ADD_LINKER_FLAGS " ${NFCONFIG_OUTPUT} " CACHE STRING "")
ENDIF ()

SET (HOMME_FIND_BLASLAPACK TRUE CACHE BOOL "")
SET (USE_QUEUING FALSE CACHE BOOL "")
