#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <sqlite3.h>

#if __has_include(<X11/Xft/Xft.h>)
  #define KD_PICKER_HAVE_XFT 1
  #include <X11/Xft/Xft.h>
#else
  #define KD_PICKER_HAVE_XFT 0
#endif

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <filesystem>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using std::filesystem::absolute;
using std::any_of;
using std::cerr;
using std::cout;
using std::function;
using std::getenv;
using std::getline;
using std::ifstream;
using std::isalpha;
using std::isspace;
using std::istringstream;
using std::isupper;
using std::make_pair;
using std::map;
using std::max;
using std::min;
using std::move;
using std::mt19937;
using std::ostringstream;
using std::pair;
using std::filesystem::path;
using std::ptrdiff_t;
using std::random_device;
using std::set;
using std::shuffle;
using std::sort;
using std::string;
using std::swap;
using std::to_string;
using std::tolower;
using std::unordered_map;
using std::vector;

[[noreturn]] static void fatal_error(const string& message) {
    cerr << "error: " << message << "\n";
    std::exit(1);
}

struct kanji_readings {

    set<string> readings;
    set<string> okurigana_stems;
};

struct kunyomi_reading_set {
    vector<string> readings;
    vector<string> okurigana_stems;
};

struct vocab_word {
    string written;
    string reading;
    string meaning;
    vector<string> kanji;
};

struct app_config {
    string anki_collection_path;
    string vocab_tsv_path;
    string kd_model_name_hint;
    string kd_deck_name_hint;
};

static string trim(const string& s) {
    const auto first = s.find_first_not_of(" \t\r\n\xef\xbb\xbf");
    if (first == string::npos) return "";
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

static string lower_ascii(string s) {
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

static bool starts_with(const string& s, const string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

static vector<string> split(const string& s, char delimiter) {
    vector<string> out;
    string part;
    istringstream in(s);
    while (getline(in, part, delimiter)) out.push_back(part);
    if (!s.empty() && s.back() == delimiter) out.emplace_back();
    return out;
}

static string strip_config_quotes(string value) {
    value = trim(value);
    if (value.size() >= 2 &&
        ((value.front() == '"' && value.back() == '"') ||
         (value.front() == '\'' && value.back() == '\''))) {
        value = value.substr(1, value.size() - 2);
    }
    return trim(value);
}

static string expand_home_prefix(const string& path) {
    if (path != "~" && !starts_with(path, "~/")) return path;
    const char* home = getenv("HOME");
    if (!home || !*home) return path;
    return string(home) + path.substr(1);
}

static string resolve_configured_path(
    const string& raw_path,
    const path& config_directory) {
    path resolved(expand_home_prefix(raw_path));
    if (resolved.is_relative()) resolved = config_directory / resolved;
    return resolved.lexically_normal().string();
}

static app_config load_config(const string& config_path) {
    const string expanded_config_path = expand_home_prefix(config_path);
    ifstream input(expanded_config_path);
    if (!input) {
        fatal_error(
            "cannot open config file '" + expanded_config_path +
            "' -- create kd_picker.conf using kd_picker.conf.example as a template.");
    }

    const path absolute_config_path =
        absolute(path(expanded_config_path)).lexically_normal();
    const path config_directory = absolute_config_path.parent_path();
    app_config config;
    bool has_anki_collection_path = false;
    bool has_vocab_tsv_path = false;
    string line;
    size_t line_number = 0;

    while (getline(input, line)) {
        ++line_number;
        line = trim(line);
        if (line.empty() || starts_with(line, "#")) continue;
        const size_t equals_pos = line.find('=');
        if (equals_pos == string::npos) {
            fatal_error("config line " + to_string(line_number) +
                                     " is missing '='.");
        }
        const string key = trim(line.substr(0, equals_pos));
        const string value = strip_config_quotes(line.substr(equals_pos + 1));

        if (key == "anki_collection_path") {
            config.anki_collection_path = resolve_configured_path(value, config_directory);
            has_anki_collection_path = !value.empty();
        } else if (key == "vocab_tsv_path") {
            config.vocab_tsv_path = resolve_configured_path(value, config_directory);
            has_vocab_tsv_path = !value.empty();
        } else if (key == "kd_model_name_hint") {
            config.kd_model_name_hint = value;
        } else if (key == "kd_deck_name_hint") {
            config.kd_deck_name_hint = value;
        } else {
            fatal_error("unknown config key '" + key + "' on line " +
                                     to_string(line_number) + ".");
        }
    }

    if (!has_anki_collection_path || config.anki_collection_path.empty())
        fatal_error("set anki_collection_path in '" + config_path + "'.");
    if (!has_vocab_tsv_path || config.vocab_tsv_path.empty())
        fatal_error("set vocab_tsv_path in '" + config_path + "'.");
    return config;
}

// convert utf-8 to code points without depending on locale.
static vector<uint32_t> utf8_to_codepoints(const string& s) {
    vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp = 0;
        size_t n = 0;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
            cp = c & 0x1F; n = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
            cp = c & 0x0F; n = 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
            cp = c & 0x07; n = 4;
        } else { out.push_back(0xFFFD); ++i; continue; }
        bool ok = true;
        for (size_t j = 1; j < n; ++j) {
            unsigned char d = static_cast<unsigned char>(s[i + j]);
            if ((d & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (d & 0x3F);
        }
        if (!ok) { out.push_back(0xFFFD); ++i; continue; }
        out.push_back(cp);
        i += n;
    }
    return out;
}

static string codepoints_to_utf8(const vector<uint32_t>& cps) {
    string out;
    for (uint32_t cp : cps) {
        if (cp <= 0x7F) out.push_back(static_cast<char>(cp));
        else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

static bool is_hiragana(uint32_t cp) { return cp >= 0x3041 && cp <= 0x3096; }
static bool is_katakana(uint32_t cp) { return (cp >= 0x30A1 && cp <= 0x30F6) || cp == 0x30FC; }
static bool is_kana(uint32_t cp) { return is_hiragana(cp) || is_katakana(cp) || cp == 0x30FC; }
static bool is_kanji(uint32_t cp) {
    return (cp >= 0x3400 && cp <= 0x4DBF) ||
           (cp >= 0x4E00 && cp <= 0x9FFF) ||
           (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0x20000 && cp <= 0x2FA1F);
}

static string normalize_reading(const string& s) {
    auto cps = utf8_to_codepoints(trim(s));
    vector<uint32_t> clean;
    for (uint32_t cp : cps) {
        if (cp == '*' || cp == 0xFF0A || cp == 0x0020 || cp == 0x3000 ||
            cp == '\t' || cp == '\r' || cp == '\n') continue;
        if (cp == 0x30FB || cp == ',' || cp == '/' || cp == ';' || cp == '|') continue;
        if (cp >= 0x30A1 && cp <= 0x30F6) cp -= 0x60;
        clean.push_back(cp);
    }
    return codepoints_to_utf8(clean);
}

static bool is_single_kanji_only(const string& word) {
    const auto cps = utf8_to_codepoints(word);
    return cps.size() == 1 && is_kanji(cps.front());
}

static bool is_valid_written_form(const string& word) {
    const auto cps = utf8_to_codepoints(word);
    if (cps.empty() || is_single_kanji_only(word)) return false;
    bool has_kanji = false;
    for (uint32_t cp : cps) {
        if (is_kanji(cp)) has_kanji = true;
        else if (!is_kana(cp)) return false;
    }
    return has_kanji;
}

static vector<string> extract_kanji(const string& word) {
    vector<string> out;
    for (uint32_t cp : utf8_to_codepoints(word)) {
        if (is_kanji(cp)) out.push_back(codepoints_to_utf8({cp}));
    }
    return out;
}

static bool all_kana(const string& s) {
    const auto cps = utf8_to_codepoints(s);
    if (cps.empty()) return false;
    for (uint32_t cp : cps) if (!is_kana(cp)) return false;
    return true;
}

static string html_to_text(const string& s) {
    string out;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '<') {
            size_t end = s.find('>', i + 1);
            if (end == string::npos) { ++i; continue; }
            string tag = lower_ascii(s.substr(i + 1, end - i - 1));
            if (starts_with(tag, "br") || starts_with(tag, "/p") || starts_with(tag, "/div") ||
                starts_with(tag, "/li") || starts_with(tag, "/tr") || starts_with(tag, "/td") ||
                starts_with(tag, "/ul") || starts_with(tag, "/ol")) out.push_back('\n');
            i = end + 1;
        } else if (s.compare(i, 6, "&nbsp;") == 0) { out.push_back(' '); i += 6; }
        else if (s.compare(i, 5, "&amp;") == 0) { out.push_back('&'); i += 5; }
        else if (s.compare(i, 4, "&lt;") == 0) { out.push_back('<'); i += 4; }
        else if (s.compare(i, 4, "&gt;") == 0) { out.push_back('>'); i += 4; }
        else if (s.compare(i, 6, "&quot;") == 0) { out.push_back('"'); i += 6; }
        else { out.push_back(s[i]); ++i; }
    }
    return out;
}

static bool cp_is_star(uint32_t cp) { return cp == '*' || cp == 0xFF0A; }

static vector<string> kana_runs(const string& input) {
    const auto cps = utf8_to_codepoints(html_to_text(input));
    vector<string> runs;
    vector<uint32_t> current;
    auto flush = [&]() {
        if (!current.empty()) {
            runs.push_back(codepoints_to_utf8(current));
            current.clear();
        }
    };
    for (uint32_t cp : cps) {
        if (is_kana(cp) || cp_is_star(cp)) current.push_back(cp);
        else flush();
    }
    flush();
    return runs;
}

static string romaji_to_hiragana(string roman) {
    roman = lower_ascii(trim(roman));
    for (char& c : roman) if (c == '-') c = ' ';
    string compact;
    for (char c : roman) if (isalpha(static_cast<unsigned char>(c))) compact.push_back(c);
    if (compact.empty()) return "";

    static const map<string, string> syllables = {
        {"a","あ"},{"i","い"},{"u","う"},{"e","え"},{"o","お"},
        {"ka","か"},{"ki","き"},{"ku","く"},{"ke","け"},{"ko","こ"},
        {"kya","きゃ"},{"kyu","きゅ"},{"kyo","きょ"},
        {"ga","が"},{"gi","ぎ"},{"gu","ぐ"},{"ge","げ"},{"go","ご"},
        {"gya","ぎゃ"},{"gyu","ぎゅ"},{"gyo","ぎょ"},
        {"sa","さ"},{"shi","し"},{"si","し"},{"su","す"},{"se","せ"},{"so","そ"},
        {"sha","しゃ"},{"shu","しゅ"},{"sho","しょ"},{"sya","しゃ"},{"syu","しゅ"},{"syo","しょ"},
        {"za","ざ"},{"ji","じ"},{"zi","じ"},{"zu","ず"},{"ze","ぜ"},{"zo","ぞ"},
        {"ja","じゃ"},{"ju","じゅ"},{"jo","じょ"},{"jya","じゃ"},{"jyu","じゅ"},{"jyo","じょ"},{"zya","じゃ"},{"zyu","じゅ"},{"zyo","じょ"},
        {"ta","た"},{"chi","ち"},{"ti","ち"},{"tsu","つ"},{"tu","つ"},{"te","て"},{"to","と"},
        {"cha","ちゃ"},{"chu","ちゅ"},{"cho","ちょ"},{"tya","ちゃ"},{"tyu","ちゅ"},{"tyo","ちょ"},
        {"da","だ"},{"di","ぢ"},{"du","づ"},{"de","で"},{"do","ど"},
        {"na","な"},{"ni","に"},{"nu","ぬ"},{"ne","ね"},{"no","の"},
        {"nya","にゃ"},{"nyu","にゅ"},{"nyo","にょ"},
        {"ha","は"},{"hi","ひ"},{"fu","ふ"},{"hu","ふ"},{"he","へ"},{"ho","ほ"},
        {"hya","ひゃ"},{"hyu","ひゅ"},{"hyo","ひょ"},
        {"ba","ば"},{"bi","び"},{"bu","ぶ"},{"be","べ"},{"bo","ぼ"},
        {"bya","びゃ"},{"byu","びゅ"},{"byo","びょ"},
        {"pa","ぱ"},{"pi","ぴ"},{"pu","ぷ"},{"pe","ぺ"},{"po","ぽ"},
        {"pya","ぴゃ"},{"pyu","ぴゅ"},{"pyo","ぴょ"},
        {"ma","ま"},{"mi","み"},{"mu","む"},{"me","め"},{"mo","も"},
        {"mya","みゃ"},{"myu","みゅ"},{"myo","みょ"},
        {"ya","や"},{"yu","ゆ"},{"yo","よ"},
        {"ra","ら"},{"ri","り"},{"ru","る"},{"re","れ"},{"ro","ろ"},
        {"rya","りゃ"},{"ryu","りゅ"},{"ryo","りょ"},
        {"wa","わ"},{"wi","うぃ"},{"we","うぇ"},{"wo","を"},
    };
    const vector<string> keys = [&]() {
        vector<string> v;
        for (const auto& p : syllables) v.push_back(p.first);
        sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.size() > b.size(); });
        return v;
    }();

    string out;
    for (size_t i = 0; i < compact.size();) {

        if (i + 1 < compact.size() && compact[i] == compact[i + 1] &&
            isalpha(static_cast<unsigned char>(compact[i])) && compact[i] != 'n') {
            out += "っ"; ++i; continue;
        }

        if (compact[i] == 'n' && (i + 1 == compact.size() ||
            compact[i + 1] == 'n' || string("aiueoy").find(compact[i + 1]) == string::npos)) {
            out += "ん";
            ++i;
            continue;
        }
        bool matched = false;
        for (const auto& key : keys) {
            if (compact.compare(i, key.size(), key) == 0) {
                out += syllables.at(key); i += key.size(); matched = true; break;
            }
        }
        if (!matched) return "";
    }
    return normalize_reading(out);
}

static vector<string> roman_tokens(const string& input) {
    vector<string> out;
    string s = html_to_text(input);
    string token;
    for (size_t i = 0; i <= s.size(); ++i) {
        char c = i < s.size() ? s[i] : ' ';
        if (isalpha(static_cast<unsigned char>(c))) token.push_back(c);
        else {
            if (!token.empty()) {

                bool upper = false;
                for (char x : token) if (isupper(static_cast<unsigned char>(x))) upper = true;
                if (upper) out.push_back(token);
                token.clear();
            }
        }
    }
    return out;
}

static vector<string> parse_onyomi(const string& field) {
    vector<string> out;
    for (const auto& run : kana_runs(field)) {
        const string r = normalize_reading(run);
        if (all_kana(r)) out.push_back(r);
    }

    if (out.empty()) {
        for (const auto& token : roman_tokens(field)) {
            string r = romaji_to_hiragana(token);
            if (!r.empty() && all_kana(r)) out.push_back(r);
        }
    }
    return out;
}

static bool token_has_star(const string& token) {
    for (uint32_t cp : utf8_to_codepoints(token)) if (cp_is_star(cp)) return true;
    return false;
}

// kanjidamage uses '*' to mark the start of okurigana.
static void add_kunyomi_token(kunyomi_reading_set& out, const string& token) {
    const auto cps = utf8_to_codepoints(token);
    size_t star_pos = cps.size();
    for (size_t i = 0; i < cps.size(); ++i) {
        if (cp_is_star(cps[i])) { star_pos = i; break; }
    }

    const string full_reading = normalize_reading(token);
    if (all_kana(full_reading)) out.readings.push_back(full_reading);

    if (star_pos < cps.size()) {
        vector<uint32_t> prefix(cps.begin(), cps.begin() + static_cast<ptrdiff_t>(star_pos));
        const string stem = normalize_reading(codepoints_to_utf8(prefix));
        if (all_kana(stem)) out.okurigana_stems.push_back(stem);
    }
}

static kunyomi_reading_set parse_kunyomi(const string& field) {
    kunyomi_reading_set out;
    const string text = html_to_text(field);
    for (const auto& line_raw : split(text, '\n')) {
        string line = trim(line_raw);
        if (line.empty()) continue;
        const auto cps = utf8_to_codepoints(line);
        vector<string> tokens;
        vector<uint32_t> current;
        bool in_paren = false;
        bool token_inside_paren = false;
        auto flush = [&]() {
            if (!current.empty()) {
                string token = codepoints_to_utf8(current);

                if (!token_inside_paren) tokens.push_back(token);
                current.clear();
                token_inside_paren = false;
            }
        };
        for (uint32_t cp : cps) {
            if (cp == '(' || cp == 0xFF08) { flush(); in_paren = true; continue; }
            if (cp == ')' || cp == 0xFF09) { flush(); in_paren = false; continue; }
            if (is_kana(cp) || cp_is_star(cp)) {
                if (current.empty()) token_inside_paren = in_paren;
                current.push_back(cp);
            } else flush();
        }
        flush();

        bool has_starred = false;
        for (const auto& token : tokens) if (token_has_star(token)) has_starred = true;
        if (has_starred) {
            for (const auto& token : tokens) if (token_has_star(token)) add_kunyomi_token(out, token);
        } else if (!tokens.empty()) {

            add_kunyomi_token(out, tokens.front());
        }
    }

    if (out.readings.empty()) {
        const auto runs = kana_runs(field);
        bool found_starred = false;
        for (const auto& token : runs) {
            if (token_has_star(token)) {
                add_kunyomi_token(out, token);
                found_starred = true;
            }
        }
        if (!found_starred && !runs.empty()) add_kunyomi_token(out, runs.front());
    }
    return out;
}

static size_t skip_ws(const string& s, size_t p) {
    while (p < s.size() && isspace(static_cast<unsigned char>(s[p]))) ++p;
    return p;
}

static bool read_json_string(const string& s, size_t& p, string& out) {
    p = skip_ws(s, p);
    if (p >= s.size() || s[p] != '"') return false;
    ++p; out.clear();
    while (p < s.size()) {
        char c = s[p++];
        if (c == '"') return true;
        if (c == '\\' && p < s.size()) {
            char e = s[p++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {

                    if (p + 4 <= s.size()) {
                        unsigned value = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = s[p++]; value <<= 4;
                            if (h >= '0' && h <= '9') value += h - '0';
                            else if (h >= 'a' && h <= 'f') value += h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F') value += h - 'A' + 10;
                        }
                        if (value < 128) out.push_back(static_cast<char>(value)); else out.push_back('?');
                    }
                    break;
                }
                default: out.push_back(e); break;
            }
        } else out.push_back(c);
    }
    return false;
}

static size_t find_json_value_start(const string& s, const string& key) {
    const string needle = "\"" + key + "\"";
    size_t p = 0;
    while ((p = s.find(needle, p)) != string::npos) {
        size_t q = skip_ws(s, p + needle.size());
        if (q < s.size() && s[q] == ':') return skip_ws(s, q + 1);
        p += needle.size();
    }
    return string::npos;
}

static size_t find_json_matching(const string& s, size_t start, char open, char close) {
    if (start >= s.size() || s[start] != open) return string::npos;
    int depth = 0; bool in_string = false; bool escaped = false;
    for (size_t i = start; i < s.size(); ++i) {
        char c = s[i];
        if (in_string) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') in_string = true;
        else if (c == open) ++depth;
        else if (c == close && --depth == 0) return i;
    }
    return string::npos;
}

struct anki_model {
    string id;
    string name;
    vector<string> fields;
};

static vector<anki_model> parse_models(const string& json) {
    vector<anki_model> models;
    size_t p = skip_ws(json, 0);
    if (p >= json.size() || json[p] != '{') return models;
    ++p;
    while (p < json.size()) {
        p = skip_ws(json, p);
        if (p >= json.size() || json[p] == '}') break;
        string id;
        if (!read_json_string(json, p, id)) break;
        p = skip_ws(json, p);
        if (p >= json.size() || json[p++] != ':') break;
        p = skip_ws(json, p);
        if (p >= json.size() || json[p] != '{') break;
        size_t end = find_json_matching(json, p, '{', '}');
        if (end == string::npos) break;
        string obj = json.substr(p, end - p + 1);
        anki_model model; model.id = id;
        size_t name_pos = find_json_value_start(obj, "name");
        if (name_pos != string::npos) read_json_string(obj, name_pos, model.name);
        size_t flds_pos = find_json_value_start(obj, "flds");
        if (flds_pos != string::npos && flds_pos < obj.size() && obj[flds_pos] == '[') {
            size_t flds_end = find_json_matching(obj, flds_pos, '[', ']');
            if (flds_end != string::npos) {
                size_t q = flds_pos + 1;
                while (q < flds_end) {
                    q = skip_ws(obj, q);
                    if (q >= flds_end) break;
                    if (obj[q] == '{') {
                        size_t fe = find_json_matching(obj, q, '{', '}');
                        if (fe == string::npos || fe > flds_end) break;
                        string field_obj = obj.substr(q, fe - q + 1);
                        size_t f_name_pos = find_json_value_start(field_obj, "name");
                        string fname;
                        if (f_name_pos != string::npos && read_json_string(field_obj, f_name_pos, fname)) model.fields.push_back(fname);
                        q = fe + 1;
                    } else ++q;
                }
            }
        }
        models.push_back(model);
        p = end + 1;
        p = skip_ws(json, p);
        if (p < json.size() && json[p] == ',') ++p;
        else if (p < json.size() && json[p] == '}') break;
    }
    return models;
}

static map<string, string> parse_decks(const string& json) {
    map<string, string> decks;
    size_t p = skip_ws(json, 0);
    if (p >= json.size() || json[p] != '{') return decks;
    ++p;
    while (p < json.size()) {
        p = skip_ws(json, p);
        if (p >= json.size() || json[p] == '}') break;
        string id;
        if (!read_json_string(json, p, id)) break;
        p = skip_ws(json, p);
        if (p >= json.size() || json[p++] != ':') break;
        p = skip_ws(json, p);
        if (p >= json.size() || json[p] != '{') break;
        size_t end = find_json_matching(json, p, '{', '}');
        if (end == string::npos) break;
        const string deck_obj = json.substr(p, end - p + 1);
        size_t name_pos = find_json_value_start(deck_obj, "name");
        string name;
        if (name_pos != string::npos && read_json_string(deck_obj, name_pos, name)) decks[id] = name;
        p = end + 1;
        p = skip_ws(json, p);
        if (p < json.size() && json[p] == ',') ++p;
        else if (p < json.size() && json[p] == '}') break;
    }
    return decks;
}

static int sqlite_unicase_compare(void*, int left_size, const void* left_data,
                                  int right_size, const void* right_data) {
    const auto* left = static_cast<const unsigned char*>(left_data);
    const auto* right = static_cast<const unsigned char*>(right_data);
    const int common_size = min(left_size, right_size);
    for (int i = 0; i < common_size; ++i) {
        unsigned char a = left[i];
        unsigned char b = right[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<unsigned char>(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = static_cast<unsigned char>(b + ('a' - 'A'));
        if (a < b) return -1;
        if (a > b) return 1;
    }
    if (left_size < right_size) return -1;
    if (left_size > right_size) return 1;
    return 0;
}

static void register_anki_collations(sqlite3* db) {
    const int rc = sqlite3_create_collation_v2(
        db, "unicase", SQLITE_UTF8, nullptr, sqlite_unicase_compare, nullptr);
    if (rc != SQLITE_OK) {
        fatal_error(string("could not register anki unicase collation: ") +
                                 sqlite3_errmsg(db));
    }
}

static bool sqlite_table_exists(sqlite3* db, const string& table_name) {
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(
        db, "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ? LIMIT 1",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, table_name.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    const bool exists = (rc == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return exists;
}

static vector<anki_model> load_models_from_db(sqlite3* db) {

    vector<anki_model> normalized_models;
    if (sqlite_table_exists(db, "notetypes") && sqlite_table_exists(db, "fields")) {
        sqlite3_stmt* stmt = nullptr;
        const char* sql =
            "SELECT n.id, n.name, f.ord, f.name "
            "FROM notetypes AS n "
            "LEFT JOIN fields AS f ON f.ntid = n.id "
            "ORDER BY n.id, f.ord";
        int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
        if (rc == SQLITE_OK) {
            unordered_map<string, size_t> index_by_id;
            while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
                const auto* name_text = sqlite3_column_text(stmt, 1);
                const string id = to_string(sqlite3_column_int64(stmt, 0));
                const string name = name_text
                    ? reinterpret_cast<const char*>(name_text) : "";
                auto it = index_by_id.find(id);
                if (it == index_by_id.end()) {
                    anki_model model;
                    model.id = id;
                    model.name = name;
                    normalized_models.push_back(move(model));
                    const size_t index = normalized_models.size() - 1;
                    index_by_id[id] = index;
                    it = index_by_id.find(id);
                }
                if (sqlite3_column_type(stmt, 2) != SQLITE_NULL) {
                    const auto* field_text = sqlite3_column_text(stmt, 3);
                    if (field_text) {
                        normalized_models[it->second].fields.emplace_back(
                            reinterpret_cast<const char*>(field_text));
                    }
                }
            }
            sqlite3_finalize(stmt);
            if (rc == SQLITE_DONE) {
                const bool has_fields = any_of(
                    normalized_models.begin(), normalized_models.end(),
                    [](const anki_model& model) { return !model.fields.empty(); });
                if (has_fields) return normalized_models;
            }
        } else if (stmt) {
            sqlite3_finalize(stmt);
        }
    }

    vector<anki_model> legacy_models;
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, "SELECT models FROM col LIMIT 1", -1, &stmt, nullptr);
    if (rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* model_text = sqlite3_column_text(stmt, 0);
        const string models_json = model_text
            ? reinterpret_cast<const char*>(model_text) : "";
        legacy_models = parse_models(models_json);
    }
    if (stmt) sqlite3_finalize(stmt);
    return legacy_models;
}

static string describe_models(const vector<anki_model>& models) {
    ostringstream out;
    if (models.empty()) return "no note types or fields could be read from this anki collection.";
    out << "note types and fields found in the collection:\n";
    for (const auto& model : models) {
        out << "  - " << (model.name.empty() ? "(unnamed note type)" : model.name)
            << " [id " << model.id << "]: ";
        if (model.fields.empty()) {
            out << "(no fields read)";
        } else {
            for (size_t i = 0; i < model.fields.size(); ++i) {
                if (i) out << ", ";
                out << model.fields[i];
            }
        }
        out << '\n';
    }
    return out.str();
}

static vector<anki_model> load_models_from_collection(const string& db_path) {
    sqlite3* db = nullptr;
    const int rc = sqlite3_open_v2(
        db_path.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr);
    if (rc != SQLITE_OK) {
        const string message = db ? sqlite3_errmsg(db) : "unable to open database";
        if (db) sqlite3_close(db);
        fatal_error("cannot open anki collection at '" + db_path + "': " + message);
    }
    register_anki_collations(db);
    const auto models = load_models_from_db(db);
    sqlite3_close(db);
    return models;
}

static map<string, string> load_decks_from_db(sqlite3* db) {

    if (sqlite_table_exists(db, "decks")) {
        set<string> columns;
        sqlite3_stmt* pragma_stmt = nullptr;
        int rc = sqlite3_prepare_v2(db, "PRAGMA table_info(decks)", -1, &pragma_stmt, nullptr);
        if (rc == SQLITE_OK) {
            while ((rc = sqlite3_step(pragma_stmt)) == SQLITE_ROW) {
                const auto* name_text = sqlite3_column_text(pragma_stmt, 1);
                if (name_text) columns.insert(lower_ascii(reinterpret_cast<const char*>(name_text)));
            }
        }
        if (pragma_stmt) sqlite3_finalize(pragma_stmt);
        if (columns.count("id") && columns.count("name")) {
            map<string, string> decks;
            sqlite3_stmt* stmt = nullptr;
            rc = sqlite3_prepare_v2(db, "SELECT CAST(id AS TEXT), name FROM decks", -1, &stmt, nullptr);
            if (rc == SQLITE_OK) {
                while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
                    const auto* id_text = sqlite3_column_text(stmt, 0);
                    const auto* name_text = sqlite3_column_text(stmt, 1);
                    if (id_text && name_text) {
                        decks[reinterpret_cast<const char*>(id_text)] =
                            reinterpret_cast<const char*>(name_text);
                    }
                }
            }
            if (stmt) sqlite3_finalize(stmt);
            if (!decks.empty()) return decks;
        }
    }

    sqlite3_stmt* stmt = nullptr;
    const int rc = sqlite3_prepare_v2(db, "SELECT decks FROM col LIMIT 1", -1, &stmt, nullptr);
    map<string, string> decks;
    if (rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
        const auto* deck_text = sqlite3_column_text(stmt, 0);
        if (deck_text) decks = parse_decks(reinterpret_cast<const char*>(deck_text));
    }
    if (stmt) sqlite3_finalize(stmt);
    return decks;
}

static map<string, string> load_decks_from_collection(const string& db_path) {
    sqlite3* db = nullptr;
    const int rc = sqlite3_open_v2(
        db_path.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr);
    if (rc != SQLITE_OK) {
        const string message = db ? sqlite3_errmsg(db) : "unable to open database";
        if (db) sqlite3_close(db);
        fatal_error("cannot open anki collection at '" + db_path + "': " + message);
    }
    register_anki_collations(db);
    const auto decks = load_decks_from_db(db);
    sqlite3_close(db);
    return decks;
}

static int find_field(const anki_model& model, const vector<string>& candidates) {
    vector<string> fields;
    for (const auto& f : model.fields) fields.push_back(lower_ascii(trim(f)));
    for (const auto& c : candidates)
        for (size_t i = 0; i < fields.size(); ++i)
            if (fields[i] == lower_ascii(c)) return static_cast<int>(i);
    return -1;
}

static vector<string> field_values(const string& flds) {
    return split(flds, '\x1f');
}

static string first_kanji_from_field(const string& field) {
    string text = html_to_text(field);
    for (uint32_t cp : utf8_to_codepoints(text)) if (is_kanji(cp)) return codepoints_to_utf8({cp});
    return "";
}

static void add_readings(kanji_readings& target, const vector<string>& readings) {
    for (const auto& r0 : readings) {
        string r = normalize_reading(r0);
        if (all_kana(r)) target.readings.insert(r);
    }
}

static void sql_check(int rc, sqlite3* db, const string& action) {
    if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW)
        fatal_error(action + ": " + (db ? sqlite3_errmsg(db) : sqlite3_errstr(rc)));
}

static map<string, kanji_readings> load_learned_kanji(
    const string& db_path, const string& model_name_hint,
    const string& deck_name_hint) {
    sqlite3* db = nullptr;
    int rc = sqlite3_open_v2(db_path.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr);
    if (rc != SQLITE_OK) {
        string message = db ? sqlite3_errmsg(db) : "unable to open database";
        if (db) sqlite3_close(db);
        fatal_error("cannot open anki collection at '" + db_path + "': " + message);
    }
    sqlite3_busy_timeout(db, 3000);
    register_anki_collations(db);

    sqlite3_stmt* stmt = nullptr;
    vector<anki_model> models = load_models_from_db(db);
    if (models.empty()) {
        sqlite3_close(db);
        fatal_error(
            "could not read anki note-type metadata. Try running "
            "'./kd_picker --list-note-types' and check your anki version/collection path.");
    }
    unordered_map<string, anki_model> model_by_id;
    unordered_map<string, string> name_by_id;
    for (const auto& m : models) { model_by_id[m.id] = m; name_by_id[m.id] = m.name; }

    struct field_layout {
        int kanji = -1;
        int on1 = -1;
        int on_full = -1;
        int kun = -1;
        int kun1 = -1;
        int kun2 = -1;
        int kun_full = -1;
        bool valid = false;
    };
    unordered_map<string, field_layout> layout_by_id;
    vector<string> selected_models;
    for (const auto& m : models) {
        field_layout f;
        f.kanji = find_field(m, {"kanji", "kanjis", "literal"});
        f.on1 = find_field(m, {"onyomi", "on-yomi"});
        f.on_full = find_field(m, {"full onyomi", "full on-yomi"});
        f.kun = find_field(m, {"kunyomi", "kun-yomi"});
        f.kun1 = find_field(m, {"first kunyomi", "first kun-yomi"});
        f.kun2 = find_field(m, {"second kunyomi", "second kun-yomi"});
        f.kun_full = find_field(m, {"full kunyomi", "full kun-yomi"});
        bool has_on = f.on1 >= 0 || f.on_full >= 0;
        bool has_kun = f.kun >= 0 || f.kun1 >= 0 || f.kun2 >= 0 || f.kun_full >= 0;
        bool name_matches = model_name_hint.empty() || lower_ascii(m.name).find(lower_ascii(model_name_hint)) != string::npos;
        f.valid = (f.kanji >= 0 && has_on && has_kun && name_matches);
        if (f.valid) selected_models.push_back(m.name);
        layout_by_id[m.id] = f;
    }
    if (selected_models.empty()) {
        const string diagnostic = describe_models(models);
        sqlite3_close(db);
        fatal_error(
            "no matching kanjidamage note type found. expected a note type with a 'Kanji' field, "
            "at least one 'Onyomi'field, and at least one 'Kunyomi' field.\n"
            "check the note type's field names or set kd_model_name_hint in kd_picker.conf.\n\n" +
            diagnostic +
            "you can also run './kd_picker --list-note-types' to list fields without launching the quiz.");
    }

    set<string> selected_deck_ids;
    if (!trim(deck_name_hint).empty()) {
        const auto decks = load_decks_from_db(db);
        const string wanted_deck_name = lower_ascii(trim(deck_name_hint));
        vector<string> matching_deck_names;
        for (const auto& entry : decks) {
            if (lower_ascii(entry.second).find(wanted_deck_name) != string::npos) {
                selected_deck_ids.insert(entry.first);
                matching_deck_names.push_back(entry.second);
            }
        }
        if (selected_deck_ids.empty()) {
            ostringstream message;
            message << "kd_deck_name_hint='" << deck_name_hint
                    << "' did not match any deck in the anki collection.";
            if (!decks.empty()) {
                message << " available decks:";
                for (const auto& entry : decks) message << "\n  - " << entry.second;
            } else {
                message << " aeck metadata could not be read from this collection.";
            }
            sqlite3_close(db);
            fatal_error(message.str());
        }
        cerr << "restricting cards to deck name match '" << deck_name_hint << "':";
        for (const auto& deck_name : matching_deck_names) cerr << " '" << deck_name << "'";
        cerr << "\n";
    }

    map<string, kanji_readings> learned;
    const char* sql = "SELECT n.mid, n.flds, c.queue, c.did FROM notes n JOIN cards c ON c.nid=n.id";
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sql_check(rc, db, "prepare anki card query");
    size_t eligible_card_rows = 0, eligible_notes = 0;
    set<pair<string, string>> seen_note_kanji;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const unsigned char* mid_text = sqlite3_column_text(stmt, 0);
        const unsigned char* flds_text = sqlite3_column_text(stmt, 1);
        const string mid = mid_text ? reinterpret_cast<const char*>(mid_text) : "";
        auto layout_it = layout_by_id.find(mid);
        if (layout_it == layout_by_id.end() || !layout_it->second.valid) continue;
        const int queue = sqlite3_column_int(stmt, 2);

        // queue 2 contains young and mature review cards.
        if (queue != 2) continue;
        if (!selected_deck_ids.empty()) {
            const string deck_id = to_string(sqlite3_column_int64(stmt, 3));
            if (!selected_deck_ids.count(deck_id)) continue;
        }

        const string flds = flds_text ? reinterpret_cast<const char*>(flds_text) : "";
        const auto values = field_values(flds);
        const field_layout& f = layout_it->second;
        if (static_cast<size_t>(f.kanji) >= values.size()) continue;
        const string kanji = first_kanji_from_field(values[f.kanji]);
        if (kanji.empty()) continue;
        ++eligible_card_rows;
        const auto marker = make_pair(mid, kanji);
        if (seen_note_kanji.insert(marker).second) ++eligible_notes;
        kanji_readings& out = learned[kanji];
        if (f.on1 >= 0 && static_cast<size_t>(f.on1) < values.size()) add_readings(out, parse_onyomi(values[f.on1]));
        if (f.on_full >= 0 && static_cast<size_t>(f.on_full) < values.size()) add_readings(out, parse_onyomi(values[f.on_full]));

        auto add_kunyomi_field = [&](int index) {
            if (index < 0 || static_cast<size_t>(index) >= values.size()) return;
            const auto parsed = parse_kunyomi(values[index]);
            add_readings(out, parsed.readings);
            for (const auto& stem0 : parsed.okurigana_stems) {
                const string stem = normalize_reading(stem0);
                if (all_kana(stem)) out.okurigana_stems.insert(stem);
            }
        };
        add_kunyomi_field(f.kun);
        add_kunyomi_field(f.kun1);
        add_kunyomi_field(f.kun2);
        add_kunyomi_field(f.kun_full);
    }
    if (rc != SQLITE_DONE) {
        string error = sqlite3_errmsg(db);
        sqlite3_finalize(stmt); sqlite3_close(db);
        fatal_error("anki card query failed: " + error);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    for (auto it = learned.begin(); it != learned.end();) {
        if (it->second.readings.empty()) it = learned.erase(it);
        else ++it;
    }
    cerr << "detected kanjidamage note types:";
    for (const auto& m : selected_models) cerr << " '" << m << "'";
    cerr << "\neligible card rows: " << eligible_card_rows
              << "; kanji with parsed readings: " << learned.size() << "\n";
    return learned;
}

static vector<vocab_word> load_vocab(const string& path) {
    ifstream in(path);
    if (!in) fatal_error("cannot open vocabulary TSV at '" + path + "'.");
    vector<vocab_word> words;
    string line;
    size_t line_no = 0;
    size_t skipped_single_kanji = 0;
    size_t skipped_written_form = 0;
    size_t skipped_reading_or_meaning = 0;
    while (getline(in, line)) {
        ++line_no;
        line = trim(line);
        if (line.empty() || starts_with(line, "#")) continue;
        auto cols = split(line, '\t');
        if (cols.size() < 3) {
            cerr << "skipping vocab line " << line_no << ": expected 3 tab-separated columns.\n";
            continue;
        }
        string written = trim(cols[0]);
        string reading = normalize_reading(cols[1]);
        string meaning = trim(cols[2]);
        if (lower_ascii(written) == "word" || written == "表記" || lower_ascii(written) == "kanji") continue;
        if (is_single_kanji_only(written)) {
            ++skipped_single_kanji;
            continue;
        }
        if (!is_valid_written_form(written)) {
            ++skipped_written_form;
            continue;
        }
        if (!all_kana(reading) || meaning.empty()) {
            ++skipped_reading_or_meaning;
            continue;
        }
        words.push_back({written, reading, meaning, extract_kanji(written)});
    }
    cerr << "vocabulary loader: accepted " << words.size()
              << "; skipped single-kanji-only entries: " << skipped_single_kanji
              << "; skipped unsupported written forms: " << skipped_written_form
              << "; skipped invalid reading/meaning: " << skipped_reading_or_meaning << "\n";
    return words;
}

static vector<string> rendaku_variants(const string& reading) {
    vector<string> out{reading};
    const vector<pair<string, string>> pairs = {
        {"か","が"},{"き","ぎ"},{"く","ぐ"},{"け","げ"},{"こ","ご"},
        {"さ","ざ"},{"し","じ"},{"す","ず"},{"せ","ぜ"},{"そ","ぞ"},
        {"た","だ"},{"ち","ぢ"},{"つ","づ"},{"て","で"},{"と","ど"},
        {"は","ば"},{"ひ","び"},{"ふ","ぶ"},{"へ","べ"},{"ほ","ぼ"},
    };
    for (const auto& p : pairs)
        if (starts_with(reading, p.first)) out.push_back(p.second + reading.substr(p.first.size()));
    return out;
}

static vector<string> reading_variants(const string& reading) {
    vector<string> out{reading};

    const vector<string> tails = {"く", "き", "つ", "ち", "ふ"};
    for (const auto& tail : tails) {
        if (reading.size() >= tail.size() && reading.compare(reading.size() - tail.size(), tail.size(), tail) == 0)
            out.push_back(reading.substr(0, reading.size() - tail.size()) + "っ");
    }
    return out;
}

static bool word_reading_covered(const vocab_word& word,
                               const map<string, kanji_readings>& learned) {
    const auto written_cps = utf8_to_codepoints(word.written);
    const size_t n = written_cps.size();
    const size_t m = word.reading.size();
    if (n == 0) return false;

    vector<vector<int>> memo(n + 1, vector<int>(m + 1, -1));
    function<bool(size_t, size_t)> solve = [&](size_t i, size_t pos) -> bool {
        if (i == n) return pos == m;
        int& state = memo[i][pos];
        if (state != -1) return state == 1;

        const uint32_t cp = written_cps[i];
        if (is_kana(cp)) {
            const string kana = normalize_reading(codepoints_to_utf8({cp}));
            if (!kana.empty() && pos + kana.size() <= m &&
                word.reading.compare(pos, kana.size(), kana) == 0 && solve(i + 1, pos + kana.size())) {
                state = 1;
                return true;
            }
            state = 0;
            return false;
        }
        if (!is_kanji(cp)) {
            state = 0;
            return false;
        }

        const string kanji = codepoints_to_utf8({cp});
        const auto learned_it = learned.find(kanji);
        if (learned_it == learned.end() || learned_it->second.readings.empty()) {
            state = 0;
            return false;
        }

        set<string> candidate_readings = learned_it->second.readings;

        if (i + 1 < n && is_kana(written_cps[i + 1])) {
            candidate_readings.insert(learned_it->second.okurigana_stems.begin(),
                                      learned_it->second.okurigana_stems.end());
        }

        for (const auto& base_reading : candidate_readings) {
            auto variants = reading_variants(base_reading);
            if (i > 0 && is_kanji(written_cps[i - 1])) {
                const auto voiced = rendaku_variants(base_reading);
                variants.insert(variants.end(), voiced.begin(), voiced.end());
            }
            for (const auto& candidate : variants) {
                if (candidate.empty() || pos + candidate.size() > m) continue;
                if (word.reading.compare(pos, candidate.size(), candidate) == 0 &&
                    solve(i + 1, pos + candidate.size())) {
                    state = 1;
                    return true;
                }
            }
        }
        state = 0;
        return false;
    };
    return solve(0, 0);
}

class picker_window {
public:
    picker_window(const vector<vocab_word>& words)
        : words(words), rng(random_device{}()) {
        if (words.empty()) fatal_error("there are no eligible vocabulary entries to display.");
        display = XOpenDisplay(nullptr);
        if (!display) fatal_error("could not open X display, is an X11 session running?");
        screen = DefaultScreen(display);
        window = XCreateSimpleWindow(display, RootWindow(display, screen), 80, 80, 900, 420, 1,
                                      BlackPixel(display, screen), WhitePixel(display, screen));
        XStoreName(display, window, "kd_picker");
        XSelectInput(display, window, ExposureMask | KeyPressMask | StructureNotifyMask);
        gc = XCreateGC(display, window, 0, nullptr);
        XSetForeground(display, gc, BlackPixel(display, screen));
#if KD_PICKER_HAVE_XFT
        xft_draw_color = make_xft_color("#111111");
        xft_draw = XftDrawCreate(display, window, DefaultVisual(display, screen), DefaultColormap(display, screen));
        font_large = XftFontOpenName(display, screen, "sans:lang=ja:size=42");
        font_medium = XftFontOpenName(display, screen, "sans:lang=ja:size=23");
        font_small = XftFontOpenName(display, screen, "sans:lang=ja:size=14");
        if (!font_large || !font_medium || !font_small) {
            close(); fatal_error("could not open Xft fonts -- install a Japanese-capable font and libxft-dev.");
        }
#else
        char** missing = nullptr;
        int missing_count = 0;
        char* default_string = nullptr;
        font_set = XCreateFontSet(display, "-*-*-medium-r-normal--24-*-*-*-*-*-iso10646-1",
                                  &missing, &missing_count, &default_string);
        if (missing) XFreeStringList(missing);
        if (!font_set) {
            close(); fatal_error("could not create an X11 font set -- install libxft-dev for reliable Japanese text rendering.");
        }
#endif
        XMapWindow(display, window);
        shuffle_bag();
        select_next(false);
    }

    ~picker_window() { close(); }

    void run() {
        bool running = true;
        while (running) {
            XEvent ev;
            XNextEvent(display, &ev);
            if (ev.type == Expose && ev.xexpose.count == 0) draw();
            else if (ev.type == ConfigureNotify) draw();
            else if (ev.type == KeyPress) {
                KeySym key = XLookupKeysym(&ev.xkey, 0);
                if (key == XK_q || key == XK_Escape) running = false;
                else if (key == XK_1) { show_reading = true; draw(); }
                else if (key == XK_2) { show_meaning = true; draw(); }
                else if (key == XK_n || key == XK_space || key == XK_Return) { select_next(true); draw(); }
                else if (key == XK_r) { show_reading = false; show_meaning = false; draw(); }
            }
        }
    }

private:
    const vector<vocab_word>& words;
    mt19937 rng;
    vector<size_t> bag;
    size_t current = 0;
    bool show_reading = false;
    bool show_meaning = false;
    Display* display = nullptr;
    int screen = 0;
    Window window = 0;
    GC gc = 0;
#if KD_PICKER_HAVE_XFT
    XftDraw* xft_draw = nullptr;
    XftFont* font_large = nullptr;
    XftFont* font_medium = nullptr;
    XftFont* font_small = nullptr;
    XftColor xft_draw_color{};
#else
    XFontSet font_set = nullptr;
#endif

#if KD_PICKER_HAVE_XFT
    XftColor make_xft_color(const char* name) {
        XftColor c{};
        XftColorAllocName(display, DefaultVisual(display, screen), DefaultColormap(display, screen), name, &c);
        return c;
    }
#endif

    void shuffle_bag() {
        bag.resize(words.size());
        for (size_t i = 0; i < words.size(); ++i) bag[i] = i;
        shuffle(bag.begin(), bag.end(), rng);
        if (bag.size() > 1 && bag.back() == current) swap(bag[0], bag.back());
    }

    void select_next(bool reset_display) {
        if (bag.empty()) shuffle_bag();
        current = bag.back();
        bag.pop_back();
        if (reset_display) { show_reading = false; show_meaning = false; }
    }

    void draw_utf8(int x, int y, const string& s, int size_type) {
#if KD_PICKER_HAVE_XFT
        XftFont* font = size_type == 0 ? font_large : (size_type == 1 ? font_medium : font_small);
        XftDrawStringUtf8(xft_draw, &xft_draw_color, font, x, y,
                          reinterpret_cast<const FcChar8*>(s.data()), static_cast<int>(s.size()));
#else
        (void)size_type;
        Xutf8DrawString(display, window, font_set, gc, x, y, s.data(), static_cast<int>(s.size()));
#endif
    }

    int estimate_text_width(const string& text, int size_type) {
#if KD_PICKER_HAVE_XFT
        XGlyphInfo extents{};
        XftFont* font = size_type == 0 ? font_large : (size_type == 1 ? font_medium : font_small);
        XftTextExtentsUtf8(display, font, reinterpret_cast<const FcChar8*>(text.data()), static_cast<int>(text.size()), &extents);
        return static_cast<int>(extents.xOff);
#else
        return static_cast<int>(utf8_to_codepoints(text).size()) * (size_type == 0 ? 28 : 16);
#endif
    }

    void draw_centered(int y, const string& text, int size_type) {
        int width = x_window_width();
        int x = max(20, (width - estimate_text_width(text, size_type)) / 2);
        draw_utf8(x, y, text, size_type);
    }

    int x_window_width() const {
        XWindowAttributes attr{};
        XGetWindowAttributes(display, window, &attr);
        return attr.width;
    }

    void draw() {
        if (!display || !window) return;
        XWindowAttributes attr{};
        XGetWindowAttributes(display, window, &attr);
        XClearWindow(display, window);
        const vocab_word& w = words[current];
        draw_centered(max(95, attr.height / 2 - 25), w.written, 0);
        int y = attr.height / 2 + 35;
        if (show_reading) draw_centered(y, "reading: " + w.reading, 1);
        else draw_centered(y, "1  reveal furigana / reading", 2);
        y += 43;
        if (show_meaning) draw_centered(y, "meaning: " + w.meaning, 1);
        else draw_centered(y, "2  reveal translation", 2);
        if (words.size() == 1) {
            draw_centered(attr.height - 82,
                          "only 1 eligible word: expand vocab.tsv or relax the filters", 2);
        } else {
            draw_centered(attr.height - 82,
                          "eligible words: " + to_string(words.size()), 2);
        }
        draw_centered(attr.height - 52, "space/n: next     r: hide answers     q: quit", 2);
    }

    void close() {
#if KD_PICKER_HAVE_XFT
        if (display) {
            if (xft_draw) { XftDrawDestroy(xft_draw); xft_draw = nullptr; }
            if (font_large) { XftFontClose(display, font_large); font_large = nullptr; }
            if (font_medium) { XftFontClose(display, font_medium); font_medium = nullptr; }
            if (font_small) { XftFontClose(display, font_small); font_small = nullptr; }
            if (xft_draw_color.pixel || xft_draw_color.color.alpha) {
                XftColorFree(display, DefaultVisual(display, screen), DefaultColormap(display, screen), &xft_draw_color);
                xft_draw_color = {};
            }
        }
#else
        if (display && font_set) { XFreeFontSet(display, font_set); font_set = nullptr; }
#endif
        if (display && gc) { XFreeGC(display, gc); gc = 0; }
        if (display && window) { XDestroyWindow(display, window); window = 0; }
        if (display) { XCloseDisplay(display); display = nullptr; }
    }
};

int main(int argc, char** argv) {
    bool list_note_types = false;
    bool list_decks = false;
    string config_path = "kd_picker.conf";
    if (argc >= 2 && (string(argv[1]) == "--list-note-types" ||
                      string(argv[1]) == "--list-decks")) {
        list_note_types = string(argv[1]) == "--list-note-types";
        list_decks = string(argv[1]) == "--list-decks";
        if (argc >= 3) config_path = argv[2];
        if (argc > 3) {
            cerr << "usage: " << argv[0] << " [--list-note-types|--list-decks [config_file]]\n";
            return 2;
        }
    } else {
        if (argc > 2) {
            cerr << "usage: " << argv[0] << " [config_file]\n";
            return 2;
        }
        if (argc == 2) config_path = argv[1];
    }
    const app_config config = load_config(config_path);
    cerr << "config file: " << absolute(expand_home_prefix(config_path)).lexically_normal().string() << "\n";

    if (list_note_types) {
        cout << describe_models(load_models_from_collection(config.anki_collection_path));
        return 0;
    }
    if (list_decks) {
        const auto decks = load_decks_from_collection(config.anki_collection_path);
        if (decks.empty()) {
            cout << "no deck names could be read from this anki collection.\n";
        } else {
            cout << "decks found in the collection (ID: name):\n";
            for (const auto& deck : decks) cout << "  " << deck.first << ": " << deck.second << "\n";
        }
        return 0;
    }

    const auto learned = load_learned_kanji(
        config.anki_collection_path, config.kd_model_name_hint, config.kd_deck_name_hint);
    const auto vocab = load_vocab(config.vocab_tsv_path);
    vector<vocab_word> eligible;
    size_t rejected_no_known_kanji = 0;
    size_t rejected_reading = 0;
    for (const auto& word : vocab) {
        bool all_known = true;
        for (const auto& kanji : word.kanji) {
            if (!learned.count(kanji) || learned.at(kanji).readings.empty()) {
                all_known = false;
                break;
            }
        }
        if (!all_known) {
            ++rejected_no_known_kanji;
            continue;
        }
        if (!word_reading_covered(word, learned)) {
            ++rejected_reading;
            continue;
        }
        eligible.push_back(word);
    }
    cerr << "vocabulary rows: " << vocab.size()
              << "; eligible words: " << eligible.size()
              << "; rejected for unknown kanji: " << rejected_no_known_kanji
              << "; rejected for unsupported reading segmentation: " << rejected_reading << "\n";
    if (eligible.empty()) {
        cerr << "no words passed the reading filter -- verify the vocab TSV, config paths, note field names, and anki review state.\n";
        return 1;
    }
    picker_window app(eligible);
    app.run();
    return 0;
}
