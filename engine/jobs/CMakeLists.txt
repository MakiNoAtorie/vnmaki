target_sources(vnmaki_engine PRIVATE job_system.cpp job_system.hpp)

# Targets without threads (e.g. a web build without pthreads) turn this off;
# the job system then runs every job inline on the submitting thread.
option(VNMAKI_ENABLE_THREADS "Run jobs on worker threads (OFF: run them inline)" ON)

if(VNMAKI_ENABLE_THREADS)
  set(THREADS_PREFER_PTHREAD_FLAG ON)
  find_package(Threads REQUIRED)
  target_link_libraries(vnmaki_engine PUBLIC Threads::Threads)
  set(vnmaki_jobs_threads 1)
else()
  set(vnmaki_jobs_threads 0)
endif()

set_property(
  SOURCE job_system.cpp TARGET_DIRECTORY vnmaki_engine
  APPEND
  PROPERTY COMPILE_DEFINITIONS VNMAKI_JOBS_THREADS=${vnmaki_jobs_threads})
