#include "TextToHeightModels.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "Widgets/ProgressDialog.hpp"
#include "slic3r/Utils/Http.hpp"

#include "libslic3r/TextToHeight.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/cstdio.hpp>

#include <openssl/evp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

#include <wx/utils.h>

namespace Slic3r { namespace GUI {

namespace {

namespace fs = boost::filesystem;

// The release the files are attached to. A new set of files gets a new tag, so a URL always means the same bytes.
const char* const RELEASE_URL = "https://github.com/dlvkz/OrcaSlicer/releases/download/text-to-height-models-v1/";

struct Asset
{
    const char* name;
    uint64_t    size;
    const char* sha256;
};

const Asset ASSETS[] = {
    {"sd-turbo-gptq.gguf", 893054656, "0d99523253b36caa52dbd8303314a37851fa0f044220e60e2d13a6c74386500d"},
    {"taesd.safetensors", 9793292, "db169d69145ec4ff064e49d99c95fa05d3eb04ee453de35824a6d0f325513549"},
    {"deepbump256.gguf", 26452288, "6946dbd701badeeb9b54b7cbe17fd21e65fb1200eae70873ebf418c10054fd3d"},
    {"LICENSE-SD-Turbo.md", 11852, "d6f6b1a4dce5c852bd6d7d9482d002baf0ccdb71e662250b73be9eec8764ee8d"},
    {"NOTICE.txt", 1697, "09da03f843a1a8b45772058d61c46cd5e1cad1b31f64f58caa99a4fa557bdee1"},
};

// Each request fetches at most this much, appended to name.part: the memory used stays small (Http keeps a response
// in memory) and a stopped download resumes from the part file.
const uint64_t CHUNK = 32ull << 20;

uint64_t total_size()
{
    uint64_t n = 0;
    for (const Asset& a : ASSETS)
        n += a.size;
    return n;
}

// Bytes still to fetch, judged by file sizes only (the checksums are checked while downloading).
uint64_t missing_size(const boost::filesystem::path& dir)
{
    uint64_t n = 0;
    for (const Asset& a : ASSETS) {
        boost::system::error_code ec;
        const auto final_path = dir / a.name, part_path = dir / (std::string(a.name) + ".part");
        if (boost::filesystem::exists(final_path, ec) && boost::filesystem::file_size(final_path, ec) == a.size && !ec)
            continue;
        const uint64_t part = boost::filesystem::exists(part_path, ec) ? boost::filesystem::file_size(part_path, ec) : 0;
        n += a.size - std::min(ec ? 0 : part, a.size);
    }
    return n;
}

bool sha256_of(const fs::path& path, std::string& hex)
{
    FILE* f = boost::nowide::fopen(path.string().c_str(), "rb");
    if (!f)
        return false;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    bool        ok  = ctx && EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1;
    std::vector<unsigned char> buf(1 << 20);
    for (size_t n; ok && (n = std::fread(buf.data(), 1, buf.size(), f)) > 0;)
        ok = EVP_DigestUpdate(ctx, buf.data(), n) == 1;
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int  len = 0;
    ok                = ok && !std::ferror(f) && EVP_DigestFinal_ex(ctx, md, &len) == 1;
    EVP_MD_CTX_free(ctx);
    std::fclose(f);
    if (ok) {
        static const char* digits = "0123456789abcdef";
        hex.clear();
        for (unsigned int i = 0; i < len; ++i) {
            hex += digits[md[i] >> 4];
            hex += digits[md[i] & 15];
        }
    }
    return ok;
}

bool is_complete(const fs::path& path, const Asset& a)
{
    boost::system::error_code ec;
    std::string               hex;
    return fs::exists(path, ec) && fs::file_size(path, ec) == a.size && !ec && sha256_of(path, hex) && hex == a.sha256;
}

// Downloads one file into dir, resuming its part file. done counts the bytes of all files so far, for the progress bar.
bool download(const Asset& a, const fs::path& dir, uint64_t before, std::atomic<uint64_t>& done, std::atomic<bool>& cancel,
              std::string& error)
{
    const fs::path final_path = dir / a.name;
    const fs::path part_path  = dir / (std::string(a.name) + ".part");
    boost::system::error_code ec;
    if (is_complete(final_path, a)) {
        done = before + a.size;
        return true;
    }
    uint64_t have = fs::exists(part_path, ec) ? fs::file_size(part_path, ec) : 0;
    if (ec || have > a.size) {
        fs::remove(part_path, ec);
        have = 0;
    }
    FILE* f = boost::nowide::fopen(part_path.string().c_str(), "ab");
    if (!f) {
        error = "cannot write " + part_path.string();
        return false;
    }
    const std::string url = std::string(RELEASE_URL) + a.name;
    while (have < a.size && !cancel) {
        const uint64_t want = std::min(CHUNK, a.size - have);
        std::string    body, err;
        // A dropped connection is retried a few times before giving up.
        for (int attempt = 0; attempt < 4 && !cancel; ++attempt) {
            body.clear();
            err.clear();
            Http::get(url)
                .size_limit(size_t(want) + 4096)
                .set_range((boost::format("%1%-%2%") % have % (have + want - 1)).str())
                .on_progress([&](Http::Progress p, bool& stop) {
                    stop = cancel.load();
                    done = before + have + std::min<uint64_t>(p.dlnow, want);
                })
                .on_complete([&](std::string b, unsigned status) {
                    if (status == 206 && b.size() == want)
                        body = std::move(b);
                    else
                        err = (boost::format("unexpected answer from the server (HTTP %1%, %2% bytes)") % status % b.size()).str();
                })
                .on_error([&](std::string, std::string e, unsigned status) {
                    err = status ? (boost::format("%1% (HTTP %2%)") % e % status).str() : e;
                })
                .perform_sync();
            if (!body.empty())
                break;
            BOOST_LOG_TRIVIAL(warning) << "Text to height models: " << a.name << " at " << have << ": " << err;
            if (!cancel)
                wxMilliSleep(1000 * (attempt + 1));
        }
        if (body.empty()) {
            error = err;
            break;
        }
        if (std::fwrite(body.data(), 1, body.size(), f) != body.size() || std::fflush(f) != 0) {
            error = "cannot write " + part_path.string() + " (is the disk full?)";
            break;
        }
        have += want;
        done = before + have;
    }
    std::fclose(f);
    if (cancel) {
        error = "cancelled";
        return false;
    }
    if (have < a.size) {
        if (error.empty())
            error = "the download of " + std::string(a.name) + " stopped";
        return false;
    }
    if (!is_complete(part_path, a)) {
        // Damaged: start this file again next time.
        fs::remove(part_path, ec);
        error = std::string(a.name) + " arrived damaged (its checksum does not match)";
        return false;
    }
    fs::remove(final_path, ec);
    fs::rename(part_path, final_path, ec);
    if (ec) {
        error = "cannot rename " + part_path.string() + ": " + ec.message();
        return false;
    }
    return true;
}

bool download_all(const fs::path& dir, std::atomic<uint64_t>& done, std::atomic<bool>& cancel, std::string& error)
{
    uint64_t before = 0;
    for (const Asset& a : ASSETS) {
        if (!download(a, dir, before, done, cancel, error))
            return false;
        before += a.size;
    }
    return true;
}

} // namespace

bool ensure_text_to_height_models(wxWindow* parent)
{
    if (!TextToHeight::available())
        return true;
    const std::string folder = TextToHeight::default_model_folder();
    if (TextToHeight::ModelFiles::in_folder(folder).missing().empty())
        return true;

    const uint64_t total = total_size();
    {
        // At least 0.01 GB, so a nearly finished download does not read "0.00 GB".
        const double gb = std::max(0.01, double(missing_size(fs::path(folder))) / 1e9);
        const wxString msg = wxString::Format(
            _L("Generate from text needs a model that is not part of the program. Download it now?\n\n"
               "Size: %.2f GB, saved in %s. It runs on your computer; nothing you type is sent anywhere.\n\n"
               "Powered by Stability AI. The model is SD-Turbo, licensed under the Stability AI Community License "
               "(saved with the model as LICENSE-SD-Turbo.md), with TAESD (MIT) and DeepBump (GPL-3.0). By downloading it you "
               "accept these licences.\n\n"
               "Source: %s"),
            gb, from_u8(folder), wxString::FromUTF8(RELEASE_URL));
        MessageDialog ask(parent, msg, _L("Model download"), wxOK | wxCANCEL | wxICON_INFORMATION);
        ask.SetButtonLabel(wxID_OK, _L("Download"), true);
        if (ask.ShowModal() != wxID_OK)
            return false;
    }

    boost::system::error_code ec;
    fs::create_directories(folder, ec);
    if (ec) {
        MessageDialog(parent, wxString::Format(_L("Cannot create the model folder %s: %s"), from_u8(folder), from_u8(ec.message())),
                      _L("Model download"), wxOK | wxICON_ERROR)
            .ShowModal();
        return false;
    }

    std::atomic<uint64_t> done{0};
    std::atomic<bool>     cancel{false}, finished{false};
    std::string           error;
    bool                  ok = false;
    std::thread           worker([&]() {
        ok       = download_all(fs::path(folder), done, cancel, error);
        finished = true;
    });
    {
        // The bytes already there when this run started (resumed or complete files) don't count towards the speed.
        const auto start      = std::chrono::steady_clock::now();
        uint64_t   start_done = 0;
        bool       started    = false;
        auto       text       = [&]() {
            const uint64_t d = done.load();
            wxString       t = wxString::Format(_L("%.0f of %.0f MB"), double(d) / 1e6, double(total) / 1e6);
            const double   s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            if (started && s > 3. && d > start_done) {
                const double left = double(total - d) / (double(d - start_done) / s);
                t += left < 90. ? wxString::Format(_L(", about %d s left"), int(left + 0.5)) :
                                  wxString::Format(_L(", about %d min left"), int(left / 60. + 0.5));
            }
            return t;
        };
        ProgressDialog progress(_L("Model download"), text(), 1000, parent, wxPD_APP_MODAL | wxPD_CAN_ABORT);
        wxGetApp().UpdateDlgDarkUI(&progress);
        while (!finished) {
            if (!started && done.load() > 0) {
                // The first progress report: skipped files and resumed parts are counted at once.
                start_done = done.load();
                started    = true;
            }
            if (!cancel && !progress.Update(int(std::min<uint64_t>(999, 1000 * done.load() / total)), text()))
                cancel = true;
            wxMilliSleep(100);
        }
    }
    worker.join();

    if (!ok) {
        BOOST_LOG_TRIVIAL(error) << "Text to height models: " << error;
        if (!cancel)
            MessageDialog(parent,
                          wxString::Format(_L("The model could not be downloaded: %s.\n\nWhat was downloaded is kept; try again later "
                                              "to continue."),
                                           from_u8(error)),
                          _L("Model download"), wxOK | wxICON_ERROR)
                .ShowModal();
        return false;
    }
    return TextToHeight::ModelFiles::in_folder(folder).missing().empty();
}

}} // namespace Slic3r::GUI
