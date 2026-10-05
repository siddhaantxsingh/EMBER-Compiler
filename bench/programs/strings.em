// Category: string building, conversion and comparison.
import strings;

fn main() {
    var total = 0;
    var parts: [String] = [];
    for i in 0..200000 {
        let s = "item-" + str(i);
        if strings.find(s, "99") >= 0 { total += len(s); }
        push(parts, s);
    }
    let joined = strings.join(parts, ",");
    let back = strings.split(joined, ",");
    print(total);
    print(len(joined));
    print(len(back));
    print(back[199999] == "item-199999");
}
