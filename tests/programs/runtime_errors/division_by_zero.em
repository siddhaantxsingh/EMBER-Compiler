fn average(xs: [Int]) -> Int {
    var s = 0;
    for x in xs { s += x; }
    return s / len(xs);
}

fn main() {
    print(average([1, 2, 3]));
    print(average([]));
}
