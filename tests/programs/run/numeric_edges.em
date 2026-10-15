// Edge cases of checked arithmetic and float semantics. Also used to check
// that the native backend matches the VM exactly (scripts/native_difftest.py).
import math;

fn ident(x: Int) -> Int { return x; }
fn fid(x: Float) -> Float { return x; }

fn main() {
    let min = ident(-9223372036854775807 - 1);
    print(min % ident(-1));
    print(ident(7) % ident(-1));
    print(min / ident(1));
    print(min % ident(2));
    print(ident(-7) % ident(2));
    let nan = fid(0.0) / fid(0.0);
    print(nan == nan);
    print(nan != nan);
    print(nan < 1.0);
    print(nan >= 1.0);
    print(fid(1.0) != fid(1.0));
    print(int(fid(9223372036854774784.0)));
    print(int(fid(-9223372036854775808.0)));
    print(int(fid(-0.9)));
    print(math.max(ident(3), ident(-4)));
    print(math.min(ident(3), ident(-4)));
    print(math.abs(ident(-5)));
    print(math.floor(fid(-2.5)));
    print(math.ceil(fid(-2.5)));
    print(math.sqrt(fid(-1.0)));
    print(math.pow(fid(2.0), fid(0.5)));
    print(-fid(0.0));
    print(true == ident(1) > ident(0));
}
