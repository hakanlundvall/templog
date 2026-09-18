# viewModel() builds TemplogViewModel reflectively through
# ViewModelProvider.AndroidViewModelFactory, which looks up the (Application)
# constructor by name, so R8 must not remove or rename it.
-keepclassmembers class * extends androidx.lifecycle.ViewModel {
    <init>(android.app.Application);
}
