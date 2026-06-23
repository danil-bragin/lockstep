#pragma once

// ProdScratchDir.hpp — Phase 7 S3. A tiny, self-cleaning scratch DIRECTORY for
// prod-disk tests and tools. It owns a UNIQUE temp directory (mkdtemp under
// $TMPDIR) and RAII-removes its files + the dir on destruction.
//
// WHY THIS LIVES UNDER providers/prod/ (and not in the test):
//   The CORE never sees a real path — the provider owns real filesystem paths
//   (cardinal rule 1). A test must not do raw file IO of its own (the forbidden-
//   call lint scans tests). mkdtemp/unlink/rmdir are filesystem-path management,
//   the prod boundary's concern, so the unique-path machinery belongs HERE in the
//   lint-exempt boundary zone — the test just asks for a path and stays pure.
//
// This is NOT a durable provider; it is scaffolding for driving ProdDisk against
// real files in a bounded, collision-free, self-cleaning way.

#include <cstdlib>  // getenv
#include <string>
#include <vector>

#include <unistd.h>   // mkdtemp, unlink, rmdir — ALLOWED only under providers/

namespace lockstep::prod {

// A unique temp directory that cleans itself up. Move-disabled; one owner.
class ProdScratchDir {
public:
    // Create a fresh unique directory under $TMPDIR (or /tmp). `tag` is folded
    // into the template for human-readable scratch names.
    explicit ProdScratchDir(const std::string& tag = "lockstep") {
        const char* tmp = std::getenv("TMPDIR");
        std::string base = (tmp != nullptr && tmp[0] != '\0') ? tmp : "/tmp";
        if (!base.empty() && base.back() == '/') {
            base.pop_back();
        }
        std::string tmpl = base + "/" + tag + "_XXXXXX";
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        const char* made = ::mkdtemp(buf.data());
        if (made != nullptr) {
            path_ = made;
        }
    }

    ProdScratchDir(const ProdScratchDir&) = delete;
    ProdScratchDir& operator=(const ProdScratchDir&) = delete;
    ProdScratchDir(ProdScratchDir&&) = delete;
    ProdScratchDir& operator=(ProdScratchDir&&) = delete;

    ~ProdScratchDir() {
        for (const std::string& f : files_) {
            ::unlink(f.c_str());
        }
        if (!path_.empty()) {
            ::rmdir(path_.c_str());
        }
    }

    [[nodiscard]] bool ok() const noexcept { return !path_.empty(); }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }

    // A unique file path inside the dir, tracked so the destructor unlinks it.
    [[nodiscard]] std::string file(const std::string& name) {
        std::string p = path_ + "/" + name;
        files_.push_back(p);
        return p;
    }

private:
    std::string path_{};
    std::vector<std::string> files_{};
};

} // namespace lockstep::prod
