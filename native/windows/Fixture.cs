using System.Drawing;
using System.Windows.Forms;

internal static class Fixture
{
    private static void Record(string kind) { Console.WriteLine($"{kind}:{DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() / 1000.0:R}"); Console.Out.Flush(); }
    public static void Run()
    {
        using var form = new Form { Text = "Arara Desktop Fixture", ClientSize = new Size(420, 420), StartPosition = FormStartPosition.Manual, Location = new Point(80, 80), KeyPreview = true };
        var text = new TextBox { AccessibleName = "Fixture input", Text = "Initial", Location = new Point(20, 25), Width = 350 };
        var button = new Button { AccessibleName = "Apply fixture", Text = "Apply fixture", Location = new Point(20, 70), Width = 180 };
        var result = new Label { AccessibleName = "Fixture result", Text = "Waiting", Location = new Point(20, 110), Width = 350 };
        var password = new TextBox { AccessibleName = "Fixture protected", Text = "fixture-secret", UseSystemPasswordChar = true, Location = new Point(220, 70), Width = 160 };
        var canvas = new Panel { Location = new Point(0, 200), Size = new Size(420, 220), BackColor = Color.CornflowerBlue, TabStop = true };
        canvas.MouseDown += (_, _) => { canvas.Focus(); Record("mouse_down"); };
        canvas.MouseUp += (_, _) => Record("mouse_up");
        form.KeyDown += (_, _) => Record("key_down");
        form.KeyUp += (_, _) => Record("key_up");
        button.Click += (_, _) => result.Text = $"Applied: {text.Text}";
        form.Controls.AddRange([text, button, result, password, canvas]);
        form.Shown += (_, _) => form.Activate();
        System.Windows.Forms.Application.Run(form);
    }
}
