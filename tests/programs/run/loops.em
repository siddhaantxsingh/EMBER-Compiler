fn main() {
    var i = 0;
    var total = 0;
    while true {
        i += 1;
        if i % 2 == 0 { continue; }
        if i > 9 { break; }
        total += i;
    }
    print(total);
    for k in 5..3 {
        print("never");
    }
    for a in 0..3 {
        for b in 0..3 {
            if b > a { break; }
            print(a * 10 + b);
        }
    }
    var xs = [1, 2, 3, 4];
    for x in xs {
        if x == 2 { pop(xs); pop(xs); }
        print(x);
    }
}
