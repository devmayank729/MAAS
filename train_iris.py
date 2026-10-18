import sys
import subprocess
import os
import platform

# Ensure scikit-learn is available on the remote machine
try:
    import sklearn
except ImportError:
    print("scikit-learn is not installed. Installing via pip on this node...")
    subprocess.check_call([sys.executable, "-m", "pip", "install", "scikit-learn"])
    import sklearn

from sklearn.datasets import load_iris
from sklearn.ensemble import RandomForestClassifier
from sklearn.model_selection import train_test_split
from sklearn.metrics import accuracy_score, classification_report

print("=" * 65)
print(f"  RANDOM FOREST TRAINING ON REMOTE NODE: {platform.node()}")
print(f"  System: {platform.platform()}")
print(f"  Python: {platform.python_version()} ({sys.executable})")
print("=" * 65)

print("\n1. Loading Iris dataset...")
iris = load_iris()
X, y = iris.data, iris.target
print(f"   Features: {iris.feature_names}")
print(f"   Classes:  {list(iris.target_names)}")
print(f"   Total samples: {len(X)}")

print("\n2. Splitting data (70% Train, 30% Test)...")
X_train, X_test, y_train, y_test = train_test_split(X, y, test_size=0.30, random_state=42)
print(f"   Training set: {len(X_train)} samples")
print(f"   Testing set:  {len(X_test)} samples")

print("\n3. Training Random Forest Classifier (100 Trees)...")
clf = RandomForestClassifier(n_estimators=100, random_state=42)
clf.fit(X_train, y_train)
print("   ✓ Training complete!")

print("\n4. Evaluating Model...")
y_pred = clf.predict(X_test)
accuracy = accuracy_score(y_test, y_pred)

print(f"\n🎯 FINAL ACCURACY: {accuracy * 100:.2f}%\n")
print("Detailed Classification Report:")
print(classification_report(y_test, y_pred, target_names=iris.target_names))
