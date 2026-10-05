// Category: floating-point arithmetic.
fn main() {
    let size = 400;
    var inside = 0;
    for py in 0..size {
        let y0 = float(py) / float(size) * 2.0 - 1.0;
        for px in 0..size {
            let x0 = float(px) / float(size) * 3.0 - 2.0;
            var x = 0.0;
            var y = 0.0;
            var it = 0;
            while it < 100 && x * x + y * y <= 4.0 {
                let xt = x * x - y * y + x0;
                y = 2.0 * x * y + y0;
                x = xt;
                it += 1;
            }
            if it == 100 { inside += 1; }
        }
    }
    print(inside);
}
