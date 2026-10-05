// Enums with payloads and exhaustive pattern matching.
enum Shape { Circle(Float), Rect(Float, Float), Triangle(Float, Float, Float) }

fn area(s: Shape) -> Float {
    return match s {
        Shape::Circle(r) => 3.141592653589793 * r * r,
        Shape::Rect(w, h) => w * h,
        Shape::Triangle(a, b, c) => heron(a, b, c),
    };
}

fn heron(a: Float, b: Float, c: Float) -> Float {
    let s = (a + b + c) / 2.0;
    return math.sqrt(s * (s - a) * (s - b) * (s - c));
}

import math;

fn main() {
    let shapes = [Shape::Circle(1.0), Shape::Rect(2.0, 3.0), Shape::Triangle(3.0, 4.0, 5.0)];
    var total = 0.0;
    for s in shapes {
        let a = area(s);
        print(str(s) + " has area " + str(a));
        total += a;
    }
    print(total);
}
