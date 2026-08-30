std::string config_revision(const std::string& text) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    std::ostringstream output;
    output << std::hex << std::setw(16) << std::setfill('0') << hash;
    return output.str();
}

FILE* open_crash_log() noexcept {
    FILE* file = nullptr;
#ifdef _WIN32
    (void)fopen_s(&file, "logs/crash.log", "a");
#else
    file = fopen("logs/crash.log", "a");
#endif
    return file;
}

void my_terminate_handler() {
    std::cerr << "=== std::terminate called ===" << std::endl;
    FILE* f = open_crash_log();
    if (f) {
        fprintf(f, "=== std::terminate called ===\n");
    }
    try {
        throw;
    } catch (const std::exception& e) {
        std::cerr << "Terminate: std::exception: " << e.what() << std::endl;
        if (f) fprintf(f, "Terminate: std::exception: %s\n", e.what());
    } catch (...) {
        std::cerr << "Terminate: unknown exception" << std::endl;
        if (f) fprintf(f, "Terminate: unknown exception\n");
    }
    if (f) fclose(f);
    std::abort();
}

void signal_handler(int sig) {
    g_stop.store(true);
    if (g_server && !g_default_model_loading.load()) g_server->stop();
    std::cerr << "\nreceived signal " << sig << ", stopping\n";
}

inferdeck::foundation::LogLevel parse_log_level(const std::string& s) {
    using inferdeck::foundation::LogLevel;
    if (s == "trace") return LogLevel::Trace;
    if (s == "debug") return LogLevel::Debug;
    if (s == "warn") return LogLevel::Warn;
    if (s == "error") return LogLevel::Error;
    if (s == "fatal") return LogLevel::Fatal;
    return LogLevel::Info;
}

foundation::Result<void> persist_state(const std::string& path,
                                       const std::string& model) {
    if (path.empty()) return foundation::Ok();
    const auto expanded = foundation::expand_user_path(fs::path(path));
    const auto parent = expanded.parent_path();
    if (!parent.empty()) {
        std::error_code error;
        fs::create_directories(parent, error);
        if (error) {
            return foundation::Err<void>(
                foundation::ErrorCode::IoError,
                "failed to create state directory: " + error.message());
        }
    }
    return foundation::save_json_file(
        expanded, nlohmann::json{{"loaded_model", model}}, true);
}

#ifdef _WIN32
LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ex) {
    DWORD code = ex->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_BREAKPOINT || code == EXCEPTION_SINGLE_STEP || code == 0x40010006 || code == 0xE06D7363) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    FILE* f = open_crash_log();
    if (f) {
        fprintf(f, "=== CRASH ===\n");
        fprintf(f, "code=0x%08lX addr=%p\n", code, ex->ExceptionRecord->ExceptionAddress);
        CONTEXT* ctx = ex->ContextRecord;
        fprintf(f, "rip=%p rsp=%p rbp=%p\n", (void*)ctx->Rip, (void*)ctx->Rsp, (void*)ctx->Rbp);
        fprintf(f, "rbx=%p rcx=%p rdx=%p rsi=%p rdi=%p\n",
                (void*)ctx->Rbx, (void*)ctx->Rcx, (void*)ctx->Rdx,
                (void*)ctx->Rsi, (void*)ctx->Rdi);
        fprintf(f, "r8=%p r9=%p r10=%p r11=%p r12=%p r13=%p r14=%p r15=%p\n",
                (void*)ctx->R8, (void*)ctx->R9, (void*)ctx->R10, (void*)ctx->R11,
                (void*)ctx->R12, (void*)ctx->R13, (void*)ctx->R14, (void*)ctx->R15);
        STACKFRAME64 frame = {};
        frame.AddrPC.Offset = ctx->Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrStack.Offset = ctx->Rsp;
        frame.AddrStack.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = ctx->Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        HANDLE proc = GetCurrentProcess();
        HANDLE thread = GetCurrentThread();
        SymInitialize(proc, NULL, TRUE);
        for (int i = 0; i < 30; ++i) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thread, &frame, ctx, NULL,
                             SymFunctionTableAccess64, SymGetModuleBase64, NULL)) break;
            if (frame.AddrPC.Offset == 0) break;
            DWORD64 disp = 0;
            SYMBOL_INFO* sym = (SYMBOL_INFO*)malloc(sizeof(SYMBOL_INFO) + 256);
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 255;
            if (SymFromAddr(proc, frame.AddrPC.Offset, &disp, sym)) {
                fprintf(f, "  #%d 0x%llx %s+0x%llx\n", i, frame.AddrPC.Offset, sym->Name, disp);
            } else {
                IMAGEHLP_MODULE64 module{};
                module.SizeOfStruct = sizeof(module);
                if (SymGetModuleInfo64(proc, frame.AddrPC.Offset, &module)) {
                    fprintf(f, "  #%d 0x%llx %s!(no symbol)\n", i, frame.AddrPC.Offset, module.ModuleName);
                } else {
                    fprintf(f, "  #%d 0x%llx (no symbol)\n", i, frame.AddrPC.Offset);
                }
            }
            free(sym);
        }
        SymCleanup(proc);
        fclose(f);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif
