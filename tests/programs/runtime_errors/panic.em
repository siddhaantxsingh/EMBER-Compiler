fn check(n: Int) {
    if n > 2 { panic("n is too big: " + str(n)); }
}

fn main() {
    for i in 0..5 {
        print(i);
        check(i);
    }
}
