{ .module = NULL,     .name = "print",             .value = BUILTIN(builtin_print)                         },
{ .module = NULL,     .name = "die",               .value = BUILTIN(builtin_die)                           },
{ .module = NULL,     .name = "read",              .value = BUILTIN(builtin_read)                          },
{ .module = NULL,     .name = "rand",              .value = BUILTIN(builtin_rand)                          },
{ .module = NULL,     .name = "int",               .value = BUILTIN(builtin_int)                           },
{ .module = NULL,     .name = "float",             .value = BUILTIN(builtin_float)                         },
{ .module = NULL,     .name = "str",               .value = BUILTIN(builtin_str)                           },
{ .module = NULL,     .name = "bool",              .value = BUILTIN(builtin_bool)                          },
{ .module = NULL,     .name = "regex",             .value = BUILTIN(builtin_regex)                         },
{ .module = NULL,     .name = "blob",              .value = BUILTIN(builtin_blob)                          },
{ .module = NULL,     .name = "min",               .value = BUILTIN(builtin_min)                           },
{ .module = NULL,     .name = "max",               .value = BUILTIN(builtin_max)                           },
{ .module = NULL,     .name = "getenv",            .value = BUILTIN(builtin_getenv)                        },
{ .module = NULL,     .name = "setenv",            .value = BUILTIN(builtin_setenv)                        },
{ .module = "os",     .name = "open",              .value = BUILTIN(builtin_os_open)                       },
{ .module = "os",     .name = "close",             .value = BUILTIN(builtin_os_close)                      },
{ .module = "os",     .name = "read",              .value = BUILTIN(builtin_os_read)                       },
{ .module = "os",     .name = "write",             .value = BUILTIN(builtin_os_write)                      },
{ .module = "os",     .name = "listdir",           .value = BUILTIN(builtin_os_listdir)                    },
{ .module = "os",     .name = "fcntl",             .value = BUILTIN(builtin_os_fcntl)                      },
{ .module = "os",     .name = "spawn",             .value = BUILTIN(builtin_os_spawn)                      },
{ .module = "os",     .name = "usleep",            .value = BUILTIN(builtin_os_usleep)                     },
{ .module = "os",     .name = "O_RDWR",            .value = INTEGER(O_RDWR)                                },
{ .module = "os",     .name = "O_CREAT",           .value = INTEGER(O_CREAT)                               },
{ .module = "os",     .name = "O_RDONLY",          .value = INTEGER(O_RDONLY)                              },
{ .module = "os",     .name = "O_WRONLY",          .value = INTEGER(O_WRONLY)                              },
{ .module = "os",     .name = "O_TRUNC",           .value = INTEGER(O_TRUNC)                               },
{ .module = "os",     .name = "O_APPEND",          .value = INTEGER(O_APPEND)                              },
{ .module = "os",     .name = "O_NONBLOCK",        .value = INTEGER(O_NONBLOCK)                            },
{ .module = "os",     .name = "F_SETFD",           .value = INTEGER(F_SETFD)                               },
{ .module = "os",     .name = "F_GETFD",           .value = INTEGER(F_GETFD)                               },
{ .module = "os",     .name = "F_GETFL",           .value = INTEGER(F_GETFL)                               },
{ .module = "os",     .name = "F_SETFL",           .value = INTEGER(F_SETFL)                               },
{ .module = "os",     .name = "F_DUPFD",           .value = INTEGER(F_DUPFD)                               },
{ .module = "os",     .name = "F_DUPFD_CLOEXEC",   .value = INTEGER(F_DUPFD_CLOEXEC)                       },
{ .module = "os",     .name = "F_GETOWN",          .value = INTEGER(F_GETOWN)                              },
{ .module = "os",     .name = "F_SETOWN",          .value = INTEGER(F_SETOWN)                              },
{ .module = "os",     .name = "F_GETPATH",         .value = INTEGER(F_GETPATH)                             },
{ .module = "os",     .name = "F_PREALLOCATE",     .value = INTEGER(F_PREALLOCATE)                         },
{ .module = "os",     .name = "F_SETSIZE",         .value = INTEGER(F_SETSIZE)                             },
{ .module = "os",     .name = "F_RDADVISE",        .value = INTEGER(F_RDADVISE)                            },
{ .module = "os",     .name = "F_RDAHEAD",         .value = INTEGER(F_RDAHEAD)                             },
{ .module = "os",     .name = "F_NOCACHE",         .value = INTEGER(F_NOCACHE)                             },
{ .module = "os",     .name = "F_LOG2PHYS",        .value = INTEGER(F_LOG2PHYS)                            },
{ .module = "os",     .name = "F_LOG2PHYS_EXT",    .value = INTEGER(F_LOG2PHYS_EXT)                        },
{ .module = "os",     .name = "F_FULLFSYNC",       .value = INTEGER(F_FULLFSYNC)                           },
{ .module = "os",     .name = "F_SETNOSIGPIPE",    .value = INTEGER(F_SETNOSIGPIPE)                        },
{ .module = "os",     .name = "F_GETNOSIGPIPE",    .value = INTEGER(F_GETNOSIGPIPE)                        },
{ .module = "errno",  .name = "get",               .value = BUILTIN(builtin_errno_get)                     },
{ .module = "errno",  .name = "str",               .value = BUILTIN(builtin_errno_str)                     },
{ .module = "errno",  .name = "ENOENT",            .value = INTEGER(ENOENT)                                },
{ .module = "errno",  .name = "ENOMEM",            .value = INTEGER(ENOMEM)                                },
{ .module = "errno",  .name = "EINVAL",            .value = INTEGER(EINVAL)                                },
{ .module = "errno",  .name = "EACCES",            .value = INTEGER(EACCES)                                },
{ .module = "errno",  .name = "EINTR",             .value = INTEGER(EINTR)                                 },
{ .module = "errno",  .name = "EAGAIN",            .value = INTEGER(EAGAIN)                                },
{ .module = "errno",  .name = "ENOTDIR",           .value = INTEGER(ENOTDIR)                               },
{ .module = "errno",  .name = "ENOSPC",            .value = INTEGER(ENOSPC)                                },
{ .module = "json",   .name = "parse",             .value = BUILTIN(builtin_json_parse)                    },
