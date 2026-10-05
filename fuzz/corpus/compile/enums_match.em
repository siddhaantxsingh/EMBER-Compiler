enum Shape { Circle(Float), Rect(Float, Float), Dot }

fn area(s: Shape) -> Float {
    return match s {
        Shape::Circle(r) => 3.0 * r * r,
        Shape::Rect(w, h) => w * h,
        Shape::Dot => 0.0,
    };
}

fn describe(n: Int) -> String {
    return match n {
        0 => "zero",
        1 => "one",
        -1 => "minus one",
        _ => "many",
    };
}

fn main() {
    let shapes = [Shape::Circle(1.0), Shape::Rect(2.0, 3.5), Shape::Dot];
    for s in shapes {
        print(area(s));
    }
    print(shapes[1]);
    print(describe(0));
    print(describe(-1));
    print(describe(7));
    match "go" {
        "stop" => { print("stopping"); },
        "go" => { print("going"); },
        _ => { print("?"); },
    }
    let b = match true { true => 1, false => 0 };
    print(b);
}
