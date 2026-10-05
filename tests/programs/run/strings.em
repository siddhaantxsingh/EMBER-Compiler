import strings;

fn main() {
    let s = "hello";
    print(len(s));
    print(s[0] + s[4]);
    print(s + ", " + "world");
    print(strings.upper(s));
    print(strings.lower("MiXeD"));
    print(strings.substr(s, 1, 3));
    print(strings.find(s, "ll"));
    print(strings.find(s, "z"));
    print(strings.split("a,b,,c", ","));
    print(len(strings.split("abc", "")));
    print(strings.join(["x", "y", "z"], "+"));
    print(strings.char_code("A", 0));
    print(strings.from_char_code(98));
    print(strings.repeat("ab", 3));
    print(str(42) + str(true) + str(1.5));
    print("tab\there");
    print("quote \" backslash \\");
    print("\u{48}\x69");
}
