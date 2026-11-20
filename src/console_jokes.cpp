#include <string_view>

#include "modules/console_jokes.hpp"

namespace modules {
namespace {

// ESP's read-only literals and tables live in mapped flash, not writable RAM.
constexpr const char* LIST_REPLIES[] = {
    "these aren't the litches you're looking for",
    "yo, where the litches at!",
    "persistent, aren't you?",
    "hi :)",
    "no",
    "NO MEANS NO",
};
constexpr const char* CD_REPLIES[] = {
    "dvd",
    "everything changes, except for this directory, bruv",
};
constexpr const char* LIGHT_REPLIES[] = {
    "All 4 lights in each of Litch's 64 chess squares are dedicated to the late Patrick Stewart",
    "THERE ARE FOUR LIGHTS!",
};
struct Reply { std::string_view command; const char* text; };
constexpr Reply REPLIES[] = {
    {"a:", "am i just a floppy disk to you?"},
    {"b:", "am i just a floppy disk to you?"},
    {"c:", "truly an awesome spectacle using the pirate's favorite letter (you thought it be ARRR, didn't ye?)"},
    {"d:", "remember mscdex.com?"},
    {"e:", "remember mscdex.com?"},
    {"f:", "fs in the chat"},
    {"n:", "no cap"},
    {"v:", "the most variant of the letters"},
    {"x:", "the coolest of the letters, also fun fact: two Vs reflecting each other across Vex's duck pond"},
    {"z:", "you mean zed, right?"},
    {"cat", "viktor"},
    {"grep", "i regularly express myself too, but sometimes it's not the right time or place"},
    {"uname", "my name litch, h'what u name?"},
    {"rm", "nuh uh"},
    {"rmdir", "NUH UH * dir"},
    {"why", "wouldn't you like to know"},
    {"answer", "42"},
    {"cp", "you wouldn't download a Litch. or maybe you would, it's open source and that's amazing. but also, you CAN perform Litchcraft. It comes highly recommended by all people known to have excellent taste."},
    {"copy", "you wouldn't download a Litch. or maybe you would, it's open source and that's amazing. but also, you CAN perform Litchcraft. It comes highly recommended by all people known to have excellent taste."},
    {"mv", "i like to move it move it"},
    {"qb", "just appreciate the symmetry in qb for a quick, basic moment."},
    {"touch", "never without my permission! u must haz multipass!"},
    {"pwd", "you're at now, now."},
    {"basename", "all, and they belong to us"},
    {"md5sum", "no, em dee awesum"},
    {"tree", "*paints happy little trees*"},
    {"whoami", "DON'T YOU KNOW WHO I AM?!"},
    {"passwd", "HACKERMAN"},
    {"groups", "yes"},
    {"users", "surprisingly unpopular in the land of TRON"},
    {"top", "current system measurements: 100% awesomeness, approaching 0% not awesomeness"},
    {"host", "is it time to start a light switch rave?"},
    {"ifconfig", "to if, or not... to if, config is the question"},
    {"mount", "permission denied, this intrusion has been !%1one$NO CARRIER"},
    {"upgrayedd", "which he spells thusly: U-P-G-R-A-Y-E-D-D"},
    {"dmesg", "deez messages on yo chin"},
    {"zip", "be careful with that!"},
    {"date", "sooner than you think"},
    {"import", "parts from japan, I hope. Overnight? 2JZ?"},
    {"exit", "this isn't a holodeck"},
    {"arch", "this isn't a holodeck"},
    {"format", "this is just how it comes #sorrynotsorry"},
    {"erase", "this command has been erased, nice going, now who's going to fix this?"},
    {"chainwax", "NOOICE"},
    {"lichess", "Vex stares intently at the situation. All involved are deeply vexed."},
    {"chess.com", "Vex stares intently at the situation. All involved are deeply vexed."},
    {"indeed", "Teal'c nods approvingly after busting through the glass"},
    {"omg", "yes, child?"},
    {"fuck", "zero given"},
    {"fucks", "zero given"},
    {"lol", "i've heard that you don't actually have to say this after every breath, lol, but here i am implementing a handler for it in real professional software. sigh. lol."},
    {"corner", "a corner? in MY litch? it's less likely than you think. we tried to cut as many as possible, professionally of course."},
    {"corners", "a corner? in MY litch? it's less likely than you think. we tried to cut as many as possible, professionally of course."},
};

} // namespace

bool ConsoleJokes::Handle(fxe::Console& console, const fxe::Console::Args& args) {
    const auto command = args.Get(0U);
    const char* reply = nullptr;
    if (command == "ls" || command == "dir") {
        reply = LIST_REPLIES[m_listReply];
        m_listReply = static_cast<uint8_t>((m_listReply + 1U) % 6U);
    } else if (command == "cd") {
        reply = CD_REPLIES[m_cdReply];
        m_cdReply ^= 1U;
    } else if (command == "lights") {
        reply = LIGHT_REPLIES[m_lightsReply];
        m_lightsReply ^= 1U;
    } else if (command == "edit" && args.Equals(1U, "autoexec.bat")) {
        reply = "well aren't we adventurous. don't forget BUGS=OFF while you're in there.";
    } else {
        for (const auto& candidate : REPLIES) {
            if (command == candidate.command) { reply = candidate.text; break; }
        }
    }
    if (reply == nullptr) return false;
    console.PrintLn("%s", reply);
    return true;
}

} // namespace modules
