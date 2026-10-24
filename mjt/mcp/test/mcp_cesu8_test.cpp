// CESU-8/WTF-8 消毒单测：JS/UTF-16 宿主按码元序列化，增补平面字符以高低
// 代理各 3 字节送达（issue #7 现场：nlohmann 316 整体拒收参数对象）。
// sanitizeCesu8Text 是 fromLibmcpJson 的消毒底座，值为/键名共用。

#include <gtest/gtest.h>

#include <string>

#include "cesu8.hpp"

namespace {

using mjt::mcp::sanitizeCesu8Text;

std::string hex(const std::string& bytes)
{
    static const char* kDigits = "0123456789abcdef";
    std::string        out;
    out.reserve(bytes.size() * 2);
    for (const char c : bytes) {
        const auto b = static_cast<unsigned char>(c);
        out += kDigits[b >> 4];
        out += kDigits[b & 0xF];
    }
    return out;
}

}   //namespace

TEST(McpCesu8Test, PassthroughsValidUtf8ByteIdentical)
{
    //Ω = CE A9；增补平面 4 字节形 F0 90 8C BB；emoji F0 9F 98 80。
    EXPECT_EQ(hex("Ω"), hex(sanitizeCesu8Text("Ω")));
    const std::string supplementary = "Ω\xF0\x90\x8C\xBB";
    EXPECT_EQ(hex(supplementary), hex(sanitizeCesu8Text(supplementary)));
    const std::string emoji = "\xF0\x9F\x98\x80 tail";
    EXPECT_EQ(hex(emoji), hex(sanitizeCesu8Text(emoji)));
    EXPECT_TRUE(sanitizeCesu8Text("").empty());
    EXPECT_EQ("plain ascii", sanitizeCesu8Text("plain ascii"));
}

TEST(McpCesu8Test, RecombinesSurrogatePairsIntoStandardUtf8)
{
    //U+1033B 的 CESU-8 形（高低代理各 3 字节）→ 4 字节标准 UTF-8。
    std::string cesu8 = "Ω\xED\xA0\x80\xED\xBC\xBB tail";
    EXPECT_EQ(hex("Ω\xF0\x90\x8C\xBB tail"), hex(sanitizeCesu8Text(cesu8)));
    //U+1F600（emoji）的 CESU-8 形：D83D DE00 → ED A0 BD ED B8 80。
    cesu8 = "\xED\xA0\xBD\xED\xB8\x80";
    EXPECT_EQ(hex("\xF0\x9F\x98\x80"), hex(sanitizeCesu8Text(cesu8)));
}

TEST(McpCesu8Test, ReplacesLoneSurrogatesWithReplacementChar)
{
    //孤立高代理（ED A0 80）与孤立低代理（ED BC BB）各自替换 U+FFFD。
    EXPECT_EQ(hex("\xEF\xBF\xBD"), hex(sanitizeCesu8Text("\xED\xA0\x80")));
    EXPECT_EQ(hex("\xEF\xBF\xBD"), hex(sanitizeCesu8Text("\xED\xBC\xBB")));
    //孤立代理不吞并后续合法字节。
    EXPECT_EQ(hex("\xEF\xBF\xBDx"), hex(sanitizeCesu8Text("\xED\xA0\x80x")));
}

TEST(McpCesu8Test, TruncatedSurrogatePairsDegradeWithoutOverrun)
{
    //尾部截断按 maximal subpart 口径逐残缺单元替换 U+FFFD，不越界读：
    //高代理（ED A0 80）+ 截断低代理（ED BC）= 两个残缺单元。
    EXPECT_EQ(hex("a\xEF\xBF\xBD\xEF\xBF\xBD"),
              hex(sanitizeCesu8Text("a\xED\xA0\x80\xED\xBC")));
    //孤 ED 无后续字节无法分类（ED 80..9F 是合法 UTF-8 首字节），穿透。
    EXPECT_EQ(hex("a\xEF\xBF\xBD\xED"), hex(sanitizeCesu8Text("a\xED\xA0\x80\xED")));
    EXPECT_EQ(hex("a\xEF\xBF\xBD"), hex(sanitizeCesu8Text("a\xED\xA0")));
}
