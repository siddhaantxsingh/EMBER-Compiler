// Reads lines from standard input until an empty line and counts words.
import strings;

fn main() {
    var lines = 0;
    var words = 0;
    var longest = "";
    while true {
        let line = input();
        if line == "" {
            break;
        }
        lines += 1;
        for w in strings.split(line, " ") {
            if w == "" {
                continue;
            }
            words += 1;
            if len(w) > len(longest) {
                longest = w;
            }
        }
    }
    print("lines: " + str(lines));
    print("words: " + str(words));
    print("longest: " + longest);
}
