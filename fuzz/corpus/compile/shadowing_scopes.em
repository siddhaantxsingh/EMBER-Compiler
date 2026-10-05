fn main() {
    let x = 1;
    {
        let x = 2;
        print(x);
    }
    print(x);
    if true {
        let x = x * 10;
        print(x);
    }
    for x in 5..7 {
        print(x);
    }
    print(x);
}
