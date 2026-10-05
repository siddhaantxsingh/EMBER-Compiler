fn main() {
    var fs: [fn() -> Int] = [];
    for i in 0..3 {
        push(fs, fn() -> Int { return i; });
    }
    var j = 0;
    while j < 3 {
        let k = j * 10;
        push(fs, fn() -> Int { return k; });
        j += 1;
    }
    for f in fs {
        print(f());
    }
}
