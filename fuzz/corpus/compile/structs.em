struct Point { x: Int, y: Int }
struct Line { from: Point, to: Point, label: String }

fn manhattan(l: Line) -> Int {
    return abs(l.to.x - l.from.x) + abs(l.to.y - l.from.y);
}

fn abs(n: Int) -> Int {
    if n < 0 { return -n; }
    return n;
}

fn main() {
    let p = Point { y: 2, x: 1 };
    let l = Line { from: p, to: Point { x: 4, y: -2 }, label: "diag" };
    print(l);
    print(manhattan(l));
    p.x = 10;
    print(l.from.x);
    l.to.y -= 1;
    print(l.to);
}
