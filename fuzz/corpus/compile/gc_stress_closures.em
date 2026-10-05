// args: --gc-stress
fn adder(n: Int) -> fn(Int) -> Int {
    return fn(x: Int) -> Int { return x + n; };
}

fn main() {
    var fs: [fn(Int) -> Int] = [];
    for i in 0..50 {
        push(fs, adder(i));
    }
    var s = 0;
    for f in fs {
        s += f(1);
    }
    print(s);
    var words: [String] = [];
    for i in 0..30 {
        push(words, "w" + str(i));
    }
    print(words[29] + words[0]);
}
