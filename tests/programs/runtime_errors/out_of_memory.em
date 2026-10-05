// args: --max-heap=2M
fn main() {
    var xs: [String] = [];
    var s = "x";
    while true {
        s = s + s;
        push(xs, s);
    }
}
