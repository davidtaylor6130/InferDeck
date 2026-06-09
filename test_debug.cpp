#include <iostream>
#include <string>
#include <vector>

struct TagDef {
    std::string text;
    bool is_thinking_start = false;
    bool is_thinking_end = false;
    bool is_tool_call_start = false;
    bool is_tool_call_end = false;
};

int main() {
    std::vector<TagDef> tags = {
        {"you", true, false, false, false},
        {"\u200B", false, true, false, false},
        {"<thinking>", true, false, false, false},
        {"</thinking>", false, true, false, false},
    };
    
    std::string input = "Hello 你you secret \u200B world";
    std::cout << "Input: " << input << std::endl;
    std::cout << "Input length: " << input.length() << std::endl;
    std::cout << "Input bytes: ";
    for (unsigned char c : input) {
        printf("%02x ", c);
    }
    std::cout << std::endl;
    
    for (const auto& tag : tags) {
        std::cout << "Tag: '" << tag.text << "' (len=" << tag.text.length() << ")" << std::endl;
        std::cout << "Tag bytes: ";
        for (unsigned char c : tag.text) {
            printf("%02x ", c);
        }
        std::cout << std::endl;
        size_t pos = input.find(tag.text);
        std::cout << "Found at pos: " << pos << std::endl;
    }
    
    return 0;
}