// Conway's game of life on a small torus.
fn step(grid: [[Bool]], n: Int) -> [[Bool]] {
    var next: [[Bool]] = [];
    for y in 0..n {
        var row: [Bool] = [];
        for x in 0..n {
            var alive = 0;
            for dy in -1..2 {
                for dx in -1..2 {
                    if dx == 0 && dy == 0 {
                        continue;
                    }
                    if grid[(y + dy + n) % n][(x + dx + n) % n] {
                        alive += 1;
                    }
                }
            }
            push(row, alive == 3 || alive == 2 && grid[y][x]);
        }
        push(next, row);
    }
    return next;
}

fn show(grid: [[Bool]]) {
    for row in grid {
        var line = "";
        for c in row {
            line = line + match c {
                true => "#",
                false => ".",
            };
        }
        print(line);
    }
}

fn main() {
    let n = 8;
    var grid: [[Bool]] = [];
    for y in 0..n {
        var row: [Bool] = [];
        for x in 0..n {
            push(row, false);
        }
        push(grid, row);
    }
    // A glider.
    grid[0][1] = true;
    grid[1][2] = true;
    grid[2][0] = true;
    grid[2][1] = true;
    grid[2][2] = true;
    for gen in 0..4 {
        grid = step(grid, n);
    }
    show(grid);
}
